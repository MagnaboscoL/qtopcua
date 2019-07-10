/******************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtOpcUa module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:COMM$
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** $QT_END_LICENSE$
**
******************************************************************************/

#include "quacppbackend.h"
#include "quacppclient.h"
#include "quacppsubscription.h"
#include "quacpputils.h"
#include "quacppvalueconverter.h"

#include <private/qopcuaclient_p.h>

#include <QtCore/QLoggingCategory>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

#include <QtNetwork/QHostInfo>

#include <uaplatformlayer.h>
#include <uasession.h>
#include <uastring.h>
#include <uadiscovery.h>
#include <uadatavalue.h>

#include <limits>

// We only undef max and do not use NOMINMAX, as the UA SDK seems to rely on it.
#ifdef max
#undef max
#endif

quint32 UACppAsyncBackend::m_numClients = 0;
bool UACppAsyncBackend::m_platformLayerInitialized = false;

using namespace UaClientSdk;

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_OPCUA_PLUGINS_UACPP)

UACppAsyncBackend::UACppAsyncBackend(QUACppClient *parent)
    : QOpcUaBackend()
    , m_clientImpl(parent)
    , m_minPublishingInterval(0)
{
    QMutexLocker locker(&m_lifecycleMutex);
    if (!m_platformLayerInitialized) {
        UaPlatformLayer::init();
        m_platformLayerInitialized = true;
    }
    m_numClients++;
    m_nativeSession = new UaSession();
}

UACppAsyncBackend::~UACppAsyncBackend()
{
    cleanupSubscriptions();

    if (m_nativeSession) {
        if (m_nativeSession->isConnected() != OpcUa_False) {
            if (m_nativeSession->serverStatus() != UaClient::ConnectionErrorApiReconnect)
                qCWarning(QT_OPCUA_PLUGINS_UACPP) << "UACPP: Deleting backend while still connected";
            ServiceSettings serviceSettings;
            m_nativeSession->disconnect(serviceSettings, OpcUa_True);
        }
        qCDebug(QT_OPCUA_PLUGINS_UACPP) << "UACPP: Deleting session";
        delete m_nativeSession;
    }
    QMutexLocker locker(&m_lifecycleMutex);
    m_numClients--;
    if (!m_numClients && m_platformLayerInitialized) {
        UaPlatformLayer::cleanup();
        m_platformLayerInitialized = false;
    }
}

void UACppAsyncBackend::connectionStatusChanged(OpcUa_UInt32 clientConnectionId, UaClientSdk::UaClient::ServerStatus serverStatus)
{
    Q_UNUSED(clientConnectionId);

    switch (serverStatus) {
    case UaClient::Disconnected:
        qCDebug(QT_OPCUA_PLUGINS_UACPP) << "Connection closed";
        emit stateAndOrErrorChanged(QOpcUaClient::Disconnected, QOpcUaClient::NoError);
        cleanupSubscriptions();
        break;
    case UaClient::Connected:
        qCDebug(QT_OPCUA_PLUGINS_UACPP) << "Connection established";
        emit stateAndOrErrorChanged(QOpcUaClient::Connected, QOpcUaClient::NoError);
        break;
    case UaClient::ConnectionWarningWatchdogTimeout:
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Unimplemented: Connection status changed to ConnectionWarningWatchdogTimeout";
        break;
    case UaClient::ConnectionErrorApiReconnect:
        qCDebug(QT_OPCUA_PLUGINS_UACPP) << "Connection status changed to ConnectionErrorApiReconnect";
        emit stateAndOrErrorChanged(QOpcUaClient::Disconnected, QOpcUaClient::ConnectionError);
        cleanupSubscriptions();
        break;
    case UaClient::ServerShutdown:
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Unimplemented: Connection status changed to ServerShutdown";
        break;
    case UaClient::NewSessionCreated:
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Unimplemented: Connection status changed to NewSessionCreated";
        break;
    }
}

void UACppAsyncBackend::browse(quint64 handle, const UaNodeId &id, const QOpcUaBrowseRequest &request)
{
    UaStatus status;
    ServiceSettings serviceSettings;
    BrowseContext browseContext;
    UaByteString continuationPoint;
    UaReferenceDescriptions referenceDescriptions;

    browseContext.referenceTypeId = UACppUtils::nodeIdFromQString(request.referenceTypeId());
    browseContext.nodeClassMask = request.nodeClassMask();
    browseContext.includeSubtype = request.includeSubtypes();
    browseContext.browseDirection = static_cast<OpcUa_BrowseDirection>(request.browseDirection());

    QStringList result;
    QVector<QOpcUaReferenceDescription> ret;
    status = m_nativeSession->browse(serviceSettings, id, browseContext, continuationPoint, referenceDescriptions);
    bool initialBrowse = true;
    do {
        if (!initialBrowse)
            status = m_nativeSession->browseNext(serviceSettings, OpcUa_False, continuationPoint, referenceDescriptions);

        if (status.isBad()) {
            qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Could not browse children.";
            break;
        }

        initialBrowse = false;

        for (quint32 i = 0; i < referenceDescriptions.length(); ++i)
        {
            const UaNodeId id(referenceDescriptions[i].NodeId.NodeId);
            const UaString uastr(id.toXmlString());
            result.append(QString::fromUtf8(uastr.toUtf8(), uastr.size()));

            QOpcUaReferenceDescription temp;
            QOpcUa::QExpandedNodeId expandedId;
            expandedId.setNamespaceUri(QString::fromUtf8(UaString(referenceDescriptions[i].NodeId.NamespaceUri).toUtf8()));
            expandedId.setServerIndex(referenceDescriptions[i].NodeId.ServerIndex);
            expandedId.setNodeId(UACppUtils::nodeIdToQString(referenceDescriptions[i].NodeId.NodeId));
            temp.setTargetNodeId(expandedId);
            expandedId.setNamespaceUri(QString::fromUtf8(UaString(referenceDescriptions[i].TypeDefinition.NamespaceUri).toUtf8()));
            expandedId.setServerIndex(referenceDescriptions[i].TypeDefinition.ServerIndex);
            expandedId.setNodeId(UACppUtils::nodeIdToQString(referenceDescriptions[i].TypeDefinition.NodeId));
            temp.setTypeDefinition(expandedId);
            temp.setRefTypeId(UACppUtils::nodeIdToQString(UaNodeId(referenceDescriptions[i].ReferenceTypeId)));
            temp.setNodeClass(static_cast<QOpcUa::NodeClass>(referenceDescriptions[i].NodeClass));
            temp.setBrowseName(QUACppValueConverter::scalarToQVariant<QOpcUa::QQualifiedName, OpcUa_QualifiedName>(
                                   &referenceDescriptions[i].BrowseName, QMetaType::Type::UnknownType).value<QOpcUa::QQualifiedName>());
            temp.setDisplayName(QUACppValueConverter::scalarToQVariant<QOpcUa::QLocalizedText, OpcUa_LocalizedText>(
                                    &referenceDescriptions[i].DisplayName, QMetaType::Type::UnknownType).value<QOpcUa::QLocalizedText>());
            temp.setIsForwardReference(referenceDescriptions[i].IsForward);
            ret.append(temp);
        }
    } while (continuationPoint.length() > 0);

    emit browseFinished(handle, ret, static_cast<QOpcUa::UaStatusCode>(status.statusCode()));
}

void UACppAsyncBackend::connectToEndpoint(const QUrl &url)
{
    UaStatus result;

    UaString uaUrl(url.toString(QUrl::RemoveUserInfo).toUtf8().constData());
    SessionConnectInfo sessionConnectInfo;
    UaString sNodeName(QHostInfo::localHostName().toUtf8().constData());

    sessionConnectInfo.sApplicationName = "QtOpcUA Unified Automation Backend";
    // Use the host name to generate a unique application URI
    sessionConnectInfo.sApplicationUri  = UaString("urn:%1:Qt:OpcUAClient").arg(sNodeName);
    sessionConnectInfo.sProductUri      = "urn:Qt:OpcUAClient";
    sessionConnectInfo.sSessionName     = sessionConnectInfo.sApplicationUri;
    sessionConnectInfo.applicationType = OpcUa_ApplicationType_Client;
    sessionConnectInfo.bAutomaticReconnect = OpcUa_False;

    SessionSecurityInfo sessionSecurityInfo;
    if (url.userName().length()) {
        UaString username(url.userName().toUtf8().constData());
        UaString password(url.password().toUtf8().constData());
        sessionSecurityInfo.setUserPasswordUserIdentity(username, password);
        if (m_disableEncryptedPasswordCheck)
            sessionSecurityInfo.disableEncryptedPasswordCheck = OpcUa_True;
    }

    result = m_nativeSession->connect(uaUrl, sessionConnectInfo, sessionSecurityInfo, this);

    if (result.isNotGood()) {
        // ### TODO: Check for bad syntax, which is the "wrong url" part
        emit stateAndOrErrorChanged(QOpcUaClient::Disconnected, QOpcUaClient::AccessDenied);
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Failed to connect: " << QString::fromUtf8(result.toString().toUtf8());
        return;
    }
}

void UACppAsyncBackend::disconnectFromEndpoint()
{
    cleanupSubscriptions();

    UaStatus result;
    ServiceSettings serviceSettings; // Default settings
    const OpcUa_Boolean deleteSubscriptions{OpcUa_True};

    result = m_nativeSession->disconnect(serviceSettings, deleteSubscriptions);
    QOpcUaClient::ClientError err = QOpcUaClient::NoError;
    if (result.isNotGood()) {
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Failed to disconnect.";
        err = QOpcUaClient::UnknownError;
    }

    emit stateAndOrErrorChanged(QOpcUaClient::Disconnected, err);
}

void UACppAsyncBackend::requestEndpoints(const QUrl &url)
{
    UaDiscovery discovery;
    ServiceSettings ServiceSettings;
    ClientSecurityInfo clientSecurityInfo;
    UaEndpointDescriptions endpoints;
    QVector<QOpcUa::QEndpointDescription> ret;

    UaStatus res = discovery.getEndpoints(ServiceSettings, UaString(url.toString(QUrl::RemoveUserInfo).toUtf8().data()), clientSecurityInfo, endpoints);

    if (res.isGood() && endpoints.length()) {
        for (size_t i = 0; i < endpoints.length() ; ++i) {
            QOpcUa::QEndpointDescription temp;
            temp.setEndpointUrl(QString::fromUtf8(UaString(endpoints[i].EndpointUrl).toUtf8()));
            temp.serverRef().setApplicationUri(QString::fromUtf8(UaString(endpoints[i].Server.ApplicationUri).toUtf8()));
            temp.serverRef().setProductUri(QString::fromUtf8(UaString(endpoints[i].Server.ProductUri).toUtf8()));
            temp.serverRef().setApplicationName(QOpcUa::QLocalizedText(QString::fromUtf8(UaString(endpoints[i].Server.ApplicationName.Locale).toUtf8()),
                                                                 QString::fromUtf8(UaString(endpoints[i].Server.ApplicationName.Text).toUtf8())));
            temp.serverRef().setApplicationType(static_cast<QOpcUa::QApplicationDescription::ApplicationType>(endpoints[i].Server.ApplicationType));
            temp.serverRef().setGatewayServerUri(QString::fromUtf8(UaString(endpoints[i].Server.GatewayServerUri).toUtf8()));
            temp.serverRef().setDiscoveryProfileUri(QString::fromUtf8(UaString(endpoints[i].Server.DiscoveryProfileUri).toUtf8()));
            for (int j = 0; j < endpoints[i].Server.NoOfDiscoveryUrls; ++j) {
                QString url = QString::fromUtf8(UaString(endpoints[i].Server.DiscoveryUrls[j]).toUtf8());
                temp.serverRef().discoveryUrlsRef().append(url);
            }
            temp.setServerCertificate(QByteArray(reinterpret_cast<char *>(endpoints[i].ServerCertificate.Data), endpoints[i].ServerCertificate.Length));
            temp.setSecurityMode(static_cast<QOpcUa::QEndpointDescription::MessageSecurityMode>(endpoints[i].SecurityMode));
            temp.setSecurityPolicyUri(QString::fromUtf8(UaString(endpoints[i].SecurityPolicyUri).toUtf8()));
            for (int j = 0; j < endpoints[i].NoOfUserIdentityTokens; ++j) {
                QOpcUa::QUserTokenPolicy policy;
                policy.setPolicyId(QString::fromUtf8(UaString(endpoints[i].UserIdentityTokens[j].PolicyId).toUtf8()));
                policy.setTokenType(static_cast<QOpcUa::QUserTokenPolicy::TokenType>(endpoints[i].UserIdentityTokens[j].TokenType));
                policy.setIssuedTokenType(QString::fromUtf8(UaString(endpoints[i].UserIdentityTokens[j].IssuedTokenType).toUtf8()));
                policy.setIssuerEndpointUrl(QString::fromUtf8(UaString(endpoints[i].UserIdentityTokens[j].IssuerEndpointUrl).toUtf8()));
                policy.setSecurityPolicyUri(QString::fromUtf8(UaString(endpoints[i].UserIdentityTokens[j].SecurityPolicyUri).toUtf8()));
                temp.userIdentityTokensRef().append(policy);
            }
            temp.setTransportProfileUri(QString::fromUtf8(UaString(endpoints[i].TransportProfileUri).toUtf8()));
            temp.setSecurityLevel(endpoints[i].SecurityLevel);
            ret.append(temp);
        }
    }

    emit endpointsRequestFinished(ret, static_cast<QOpcUa::UaStatusCode>(res.code()));
}

inline OpcUa_UInt32 toUaAttributeId(QOpcUa::NodeAttribute attr)
{
    const int attributeIdUsedBits = 22;
    for (int i = 0; i < attributeIdUsedBits; ++i)
        if (static_cast<int>(attr) == (1 << i))
            return static_cast<OpcUa_UInt32>(i + 1);

    return static_cast<OpcUa_UInt32>(0);
}

void UACppAsyncBackend::readAttributes(quint64 handle, const UaNodeId &id, QOpcUa::NodeAttributes attr, QString indexRange)
{
    UaStatus result;

    ServiceSettings settings;
    UaReadValueIds nodeToRead;
    UaDataValues values;
    UaDiagnosticInfos diagnosticInfos;

    QVector<QOpcUaReadResult> vec;

    int attributeSize = 0;

    qt_forEachAttribute(attr, [&](QOpcUa::NodeAttribute attribute){
        attributeSize++;
        nodeToRead.resize(attributeSize);
        id.copyTo(&nodeToRead[attributeSize - 1].NodeId);
        nodeToRead[attributeSize - 1].AttributeId = toUaAttributeId(attribute);
        if (indexRange.size()) {
            UaString ir(indexRange.toUtf8().constData());
            ir.copyTo(&nodeToRead[attributeSize - 1].IndexRange);
        }
        QOpcUaReadResult temp;
        temp.setAttribute(attribute);
        vec.push_back(temp);
    });

    result = m_nativeSession->read(settings,
                                   0,
                                   OpcUa_TimestampsToReturn_Both,
                                   nodeToRead,
                                   values,
                                   diagnosticInfos);
    if (result.isBad()) {
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Reading attributes failed:" << result.toString().toUtf8();
    } else {
        for (int i = 0; i < vec.size(); ++i) {
            vec[i].setStatusCode(static_cast<QOpcUa::UaStatusCode>(values[i].StatusCode));
            vec[i].setValue(QUACppValueConverter::toQVariant(values[i].Value));
            vec[i].setServerTimestamp(QUACppValueConverter::toQDateTime(&values[i].ServerTimestamp));
            vec[i].setSourceTimestamp(QUACppValueConverter::toQDateTime(&values[i].SourceTimestamp));
        }
    }

    emit attributesRead(handle, vec, static_cast<QOpcUa::UaStatusCode>(result.statusCode()));
}

void UACppAsyncBackend::writeAttribute(quint64 handle, const UaNodeId &id, QOpcUa::NodeAttribute attrId, QVariant value, QOpcUa::Types type, QString indexRange)
{
    if (type == QOpcUa::Types::Undefined && attrId != QOpcUa::NodeAttribute::Value)
        type = attributeIdToTypeId(attrId);

    UaStatus result;
    ServiceSettings settings;
    UaWriteValues nodesToWrite;
    UaStatusCodeArray writeResults;
    UaDiagnosticInfos diagnosticInfos;

    nodesToWrite.create(1);
    id.copyTo(&nodesToWrite[0].NodeId);
    nodesToWrite[0].AttributeId = QUACppValueConverter::toUaAttributeId(attrId);
    nodesToWrite[0].Value.Value = QUACppValueConverter::toUACppVariant(value, type);
    if (indexRange.size()) {
        UaString ir(indexRange.toUtf8());
        ir.copyTo(&nodesToWrite[0].IndexRange);
    }
    result = m_nativeSession->write(settings, nodesToWrite, writeResults, diagnosticInfos);

    if (result.isBad())
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Writing attribute failed:" << result.toString().toUtf8();

    emit attributeWritten(handle, attrId, result.isGood() ? value : QVariant(), writeResults.length() ?
                              static_cast<QOpcUa::UaStatusCode>(writeResults[0]) : static_cast<QOpcUa::UaStatusCode>(result.statusCode()));
}

void UACppAsyncBackend::writeAttributes(quint64 handle, const UaNodeId &id, QOpcUaNode::AttributeMap toWrite, QOpcUa::Types valueAttributeType)
{
    if (toWrite.size() == 0) {
        qCWarning(QT_OPCUA_PLUGINS_UACPP, "No values to be written");
        emit attributeWritten(handle, QOpcUa::NodeAttribute::None, QVariant(), QOpcUa::UaStatusCode::BadNothingToDo);
        return;
    }

    UaStatus result;
    ServiceSettings settings;
    UaWriteValues nodesToWrite;
    UaStatusCodeArray writeResults;
    UaDiagnosticInfos diagnosticInfos;

    nodesToWrite.create(toWrite.size());
    quint32 index = 0;
    for (auto it = toWrite.constBegin(); it != toWrite.constEnd(); ++it, ++index) {
        id.copyTo(&nodesToWrite[index].NodeId);
        QOpcUa::Types type = attributeIdToTypeId(it.key());
        if (type == QOpcUa::Types::Undefined)
            type = valueAttributeType;
        nodesToWrite[index].AttributeId = QUACppValueConverter::toUaAttributeId(it.key());
        nodesToWrite[index].Value.Value = QUACppValueConverter::toUACppVariant(it.value(), type);
    }

    result = m_nativeSession->write(settings, nodesToWrite, writeResults, diagnosticInfos);

    if (result.isBad())
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Writing attribute failed:" << result.toString().toUtf8();

    index = 0;
    for (auto it = toWrite.constBegin(); it != toWrite.constEnd(); ++it, ++index) {
        QOpcUa::UaStatusCode status = index < writeResults.length() ?
                    static_cast<QOpcUa::UaStatusCode>(writeResults[index]) : static_cast<QOpcUa::UaStatusCode>(result.statusCode());
        emit attributeWritten(handle, it.key(), it.value(), status);
    }
}

void UACppAsyncBackend::enableMonitoring(quint64 handle, const UaNodeId &id, QOpcUa::NodeAttributes attr, const QOpcUaMonitoringParameters &settings)
{
    QUACppSubscription *usedSubscription = nullptr;

    // Create a new subscription if necessary
    if (settings.subscriptionId()) {
        auto sub = m_subscriptions.find(settings.subscriptionId());
        if (sub == m_subscriptions.end()) {
            qCWarning(QT_OPCUA_PLUGINS_UACPP, "There is no subscription with id %u", settings.subscriptionId());

            qt_forEachAttribute(attr, [&](QOpcUa::NodeAttribute attribute){
                QOpcUaMonitoringParameters s;
                s.setStatusCode(QOpcUa::UaStatusCode::BadSubscriptionIdInvalid);
                emit monitoringEnableDisable(handle, attribute, true, s);
            });
            return;
        }
        usedSubscription = sub.value(); // Ignore interval != subscription.interval
    } else {
        usedSubscription = getSubscription(settings);
    }

    if (!usedSubscription) {
        qCWarning(QT_OPCUA_PLUGINS_UACPP, "Could not create subscription with interval %f", settings.publishingInterval());
        qt_forEachAttribute(attr, [&](QOpcUa::NodeAttribute attribute){
            QOpcUaMonitoringParameters s;
            s.setStatusCode(QOpcUa::UaStatusCode::BadSubscriptionIdInvalid);
            emit monitoringEnableDisable(handle, attribute, true, s);
        });
        return;
    }

    qt_forEachAttribute(attr, [&](QOpcUa::NodeAttribute attribute){
        if (getSubscriptionForItem(handle, attribute)) {
            qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Monitored item for" << attribute << "has already been created";
            QOpcUaMonitoringParameters s;
            s.setStatusCode(QOpcUa::UaStatusCode::BadEntryExists);
            emit monitoringEnableDisable(handle, attribute, true, s);
        } else {
            bool success = usedSubscription->addAttributeMonitoredItem(handle, attribute, id, settings);
            if (success)
                m_attributeMapping[handle][attribute] = usedSubscription;
        }
    });

    if (usedSubscription->monitoredItemsCount() == 0)
        removeSubscription(usedSubscription->subscriptionId()); // No items were added
}

void UACppAsyncBackend::modifyMonitoring(quint64 handle, QOpcUa::NodeAttribute attr, QOpcUaMonitoringParameters::Parameter item, QVariant value)
{
    QUACppSubscription *subscription = getSubscriptionForItem(handle, attr);
    if (!subscription) {
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Could not modify" << item << ", the monitored item does not exist";
        QOpcUaMonitoringParameters p;
        p.setStatusCode(QOpcUa::UaStatusCode::BadMonitoredItemIdInvalid);
        emit monitoringStatusChanged(handle, attr, item, p);
        return;
    }

    subscription->modifyMonitoring(handle, attr, item, value);
}

void UACppAsyncBackend::disableMonitoring(quint64 handle, QOpcUa::NodeAttributes attr)
{
    qt_forEachAttribute(attr, [&](QOpcUa::NodeAttribute attribute){
        QUACppSubscription *sub = getSubscriptionForItem(handle, attribute);
        if (sub) {
            sub->removeAttributeMonitoredItem(handle, attribute);
            if (sub->monitoredItemsCount() == 0)
                removeSubscription(sub->subscriptionId());
        }
    });
}

void UACppAsyncBackend::callMethod(quint64 handle, const UaNodeId &objectId, const UaNodeId &methodId, QVector<QOpcUa::TypedVariant> args)
{
    ServiceSettings settings;
    CallIn in;

    in.objectId = objectId;
    in.methodId = methodId;

    if (args.size()) {
        in.inputArguments.resize(args.size());
        for (int i = 0; i < args.size(); ++i)
            in.inputArguments[i] = QUACppValueConverter::toUACppVariant(args[i].first, args[i].second);
    }

    CallOut out;

    UaStatus status = m_nativeSession->call(settings, in, out);
    if (status.isBad())
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Calling method failed";

    if (out.callResult.isBad())
        qCWarning(QT_OPCUA_PLUGINS_UACPP) << "Method call returned a failure";

    QVariant result;

    if (out.outputArguments.length() > 1) {
        QVariantList resultList;
        for (quint32 i = 0; i < out.outputArguments.length(); ++i)
            resultList.append(QUACppValueConverter::toQVariant(out.outputArguments[i]));
        result = resultList;
    } else if (out.outputArguments.length() == 1) {
        result = QUACppValueConverter::toQVariant(out.outputArguments[0]);
    }

    emit methodCallFinished(handle, UACppUtils::nodeIdToQString(methodId), result, static_cast<QOpcUa::UaStatusCode>(status.statusCode()));
}

void UACppAsyncBackend::resolveBrowsePath(quint64 handle, const UaNodeId &startNode, const QVector<QOpcUa::QRelativePathElement> &path)
{
    ServiceSettings settings;
    UaDiagnosticInfos diagnosticInfos;
    UaBrowsePaths paths;
    UaBrowsePathResults result;
    UaRelativePathElements pathElements;

    paths.create(1);
    startNode.copyTo(&paths[0].StartingNode);
    pathElements.create(path.size());

    for (int i = 0; i < path.size(); ++i) {
        pathElements[i].IncludeSubtypes = path[i].includeSubtypes();
        pathElements[i].IsInverse = path[i].isInverse();
        UaNodeId(UACppUtils::nodeIdFromQString(path[i].referenceTypeId())).copyTo(&pathElements[i].ReferenceTypeId);
        UaQualifiedName(UaString(path[i].targetName().name().toUtf8().constData()), path[i].targetName().namespaceIndex()).copyTo(&pathElements[i].TargetName);
    }

    paths[0].RelativePath.Elements = pathElements.detach();
    paths[0].RelativePath.NoOfElements = path.size();

    UaStatusCode serviceResult = m_nativeSession->translateBrowsePathsToNodeIds(settings, paths, result, diagnosticInfos);
    QOpcUa::UaStatusCode status = static_cast<QOpcUa::UaStatusCode>(serviceResult.code());

    QVector<QOpcUa::QBrowsePathTarget> ret;

    if (status == QOpcUa::UaStatusCode::Good && result.length()) {
        status = static_cast<QOpcUa::UaStatusCode>(result[0].StatusCode);
        for (int i = 0; i < result[0].NoOfTargets; ++i) {
            QOpcUa::QBrowsePathTarget temp;
            temp.setRemainingPathIndex(result[0].Targets[i].RemainingPathIndex);
            temp.targetIdRef().setNamespaceUri(QString::fromUtf8(UaString(result[0].Targets[i].TargetId.NamespaceUri).toUtf8()));
            temp.targetIdRef().setServerIndex(result[0].Targets[i].TargetId.ServerIndex);
            temp.targetIdRef().setNodeId(UACppUtils::nodeIdToQString(result[0].Targets[i].TargetId.NodeId));
            ret.append(temp);
        }
    }

    emit resolveBrowsePathFinished(handle, ret, path, status);
}

QUACppSubscription *UACppAsyncBackend::getSubscription(const QOpcUaMonitoringParameters &settings)
{
    if (settings.subscriptionType() == QOpcUaMonitoringParameters::SubscriptionType::Shared) {
        // Requesting multiple subscriptions with publishing interval < minimum publishing interval breaks subscription sharing
        double interval = revisePublishingInterval(settings.publishingInterval(), m_minPublishingInterval);
        for (auto entry : qAsConst(m_subscriptions)) {
            if (qFuzzyCompare(entry->interval(), interval) && entry->shared() == QOpcUaMonitoringParameters::SubscriptionType::Shared)
                return entry;
        }
    }

    QUACppSubscription *sub = new QUACppSubscription(this, settings);
    quint32 id = sub->createOnServer();
    if (!id) {
        delete sub;
        return nullptr;
    }
    if (sub->interval() > settings.publishingInterval()) // The publishing interval has been revised by the server.
        m_minPublishingInterval = sub->interval();
    m_subscriptions[id] = sub;
    return sub;
}

QUACppSubscription *UACppAsyncBackend::getSubscriptionForItem(quint64 handle, QOpcUa::NodeAttribute attr)
{
    auto entriesForHandle = m_attributeMapping.find(handle);
    if (entriesForHandle == m_attributeMapping.end())
        return nullptr;
    auto subscription = entriesForHandle->find(attr);
    if (subscription == entriesForHandle->end())
        return nullptr;

    return subscription.value();
}

void UACppAsyncBackend::cleanupSubscriptions()
{
    qDeleteAll(m_subscriptions);
    m_subscriptions.clear();
    m_attributeMapping.clear();
    m_minPublishingInterval = 0;
}

bool UACppAsyncBackend::removeSubscription(quint32 subscriptionId)
{
    auto sub = m_subscriptions.find(subscriptionId);
    if (sub != m_subscriptions.end()) {
        sub.value()->removeOnServer();
        delete sub.value();
        m_subscriptions.remove(subscriptionId);
        return true;
    }
    return false;
}

QT_END_NAMESPACE
