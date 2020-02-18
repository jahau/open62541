/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2017 (c) Mark Giraud, Fraunhofer IOSB
 *    Copyright 2017-2018 (c) Thomas Stalder, Blue Time Concept SA
 *    Copyright 2017-2019 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2018 (c) Kalycito Infotech Private Limited
 */

#include <open62541/transport_generated.h>
#include <open62541/transport_generated_encoding_binary.h>
#include <open62541/transport_generated_handling.h>
#include <open62541/types_generated_encoding_binary.h>

#include "ua_client_internal.h"

/* Size are refered in bytes */
#define UA_MINMESSAGESIZE                8192
#define UA_SESSION_LOCALNONCELENGTH      32
#define MAX_DATA_SIZE                    4096

 /********************/
 /* Set client state */
 /********************/
void
setClientState(UA_Client *client, UA_ClientState state) {
    if(client->state != state) {
        client->state = state;
        if(client->config.stateCallback)
            client->config.stateCallback(client, client->state);
    }
}

/***********************/
/* Open the Connection */
/***********************/

#define UA_BITMASK_MESSAGETYPE 0x00ffffffu
#define UA_BITMASK_CHUNKTYPE 0xff000000u

static void
processACKResponse(void *application, UA_SecureChannel *channel,
                   UA_MessageType messageType, UA_UInt32 requestId,
                   UA_ByteString *chunk) {
    UA_Client *client = (UA_Client*)application;

    /* Decode the ACK message */
    size_t offset = 8; /* Skip the header */
    UA_TcpAcknowledgeMessage ackMessage;
    UA_StatusCode retval = UA_TcpAcknowledgeMessage_decodeBinary(chunk, &offset, &ackMessage);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_NETWORK,
                     "Decoding ACK message failed");
        UA_Client_disconnect(client);
        return;
    }
    UA_LOG_DEBUG(&client->config.logger, UA_LOGCATEGORY_NETWORK, "Received ACK message");

    /* Process the ACK message */
    retval = UA_SecureChannel_processHELACK(channel, &ackMessage);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_NETWORK,
                     "Processing the ACK message failed with StatusCode %s",
                     UA_StatusCode_name(retval));
        UA_Client_disconnect(client);
    }
}

static UA_StatusCode
HelAckHandshake(UA_Client *client, const UA_String endpointUrl) {
    /* Get a buffer */
    UA_ByteString message;
    UA_Connection *conn = &client->connection;
    UA_StatusCode retval = conn->getSendBuffer(conn, UA_MINMESSAGESIZE, &message);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Prepare the HEL message and encode at offset 8 */
    UA_TcpHelloMessage hello;
    hello.protocolVersion = 0;
    hello.receiveBufferSize = client->config.localConnectionConfig.recvBufferSize;
    hello.sendBufferSize = client->config.localConnectionConfig.sendBufferSize;
    hello.maxMessageSize = client->config.localConnectionConfig.localMaxMessageSize;
    hello.maxChunkCount = client->config.localConnectionConfig.localMaxChunkCount;
    hello.endpointUrl = endpointUrl;

    UA_Byte *bufPos = &message.data[8]; /* skip the header */
    const UA_Byte *bufEnd = &message.data[message.length];
    retval = UA_TcpHelloMessage_encodeBinary(&hello, &bufPos, bufEnd);
    if(retval != UA_STATUSCODE_GOOD) {
        conn->releaseSendBuffer(conn, &message);
        return retval;
    }

    /* Encode the message header at offset 0 */
    UA_TcpMessageHeader messageHeader;
    messageHeader.messageTypeAndChunkType = UA_CHUNKTYPE_FINAL + UA_MESSAGETYPE_HEL;
    messageHeader.messageSize = (UA_UInt32)((uintptr_t)bufPos - (uintptr_t)message.data);
    bufPos = message.data;
    retval = UA_TcpMessageHeader_encodeBinary(&messageHeader, &bufPos, bufEnd);
    if(retval != UA_STATUSCODE_GOOD) {
        conn->releaseSendBuffer(conn, &message);
        return retval;
    }

    /* Send the HEL message */
    message.length = messageHeader.messageSize;
    retval = conn->send(conn, &message);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_NETWORK,
                     "Sending HEL failed");
        return retval;
    }
    UA_LOG_DEBUG(&client->config.logger, UA_LOGCATEGORY_NETWORK,
                 "Sent HEL message");

    /* Loop until we have a complete chunk */
    retval = UA_SecureChannel_receiveChunksBlocking(&client->channel, client,
                                                    processACKResponse, client->config.timeout);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_NETWORK,
                     "Receiving ACK message failed with %s", UA_StatusCode_name(retval));
        if(retval == UA_STATUSCODE_BADCONNECTIONCLOSED)
            client->state = UA_CLIENTSTATE_DISCONNECTED;
        UA_Client_disconnect(client);
    }
    return retval;
}

UA_SecurityPolicy *
getSecurityPolicy(UA_Client *client, UA_String policyUri) {
    for(size_t i = 0; i < client->config.securityPoliciesSize; i++) {
        if(UA_String_equal(&policyUri, &client->config.securityPolicies[i].policyUri))
            return &client->config.securityPolicies[i];
    }
    return NULL;
}

UA_StatusCode
openSecureChannel(UA_Client *client, UA_Boolean renew) {
    /* Check if sc is still valid */
    if(renew && client->nextChannelRenewal > UA_DateTime_nowMonotonic())
        return UA_STATUSCODE_GOOD;

    UA_Connection *conn = &client->connection;
    if(conn->state != UA_CONNECTION_ESTABLISHED)
        return UA_STATUSCODE_BADSERVERNOTCONNECTED;

    /* Generate clientNonce. */
    UA_StatusCode retval = UA_SecureChannel_generateLocalNonce(&client->channel);
    if(retval != UA_STATUSCODE_GOOD) {
      UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
        "Generating a local nonce failed");
      return retval;
    }

    /* Prepare the OpenSecureChannelRequest */
    UA_OpenSecureChannelRequest opnSecRq;
    UA_OpenSecureChannelRequest_init(&opnSecRq);
    opnSecRq.requestHeader.timestamp = UA_DateTime_now();
    opnSecRq.requestHeader.authenticationToken = client->authenticationToken;
    if(renew) {
        opnSecRq.requestType = UA_SECURITYTOKENREQUESTTYPE_RENEW;
        UA_LOG_DEBUG_CHANNEL(&client->config.logger, &client->channel,
                             "Requesting to renew the SecureChannel");
    } else {
        opnSecRq.requestType = UA_SECURITYTOKENREQUESTTYPE_ISSUE;
        UA_LOG_DEBUG_CHANNEL(&client->config.logger, &client->channel,
                             "Requesting to open a SecureChannel");
    }

    /* Set the securityMode to input securityMode from client data */
    opnSecRq.securityMode = client->channel.securityMode;

    opnSecRq.clientNonce = client->channel.localNonce;
    opnSecRq.requestedLifetime = client->config.secureChannelLifeTime;

    /* Send the OPN message */
    UA_UInt32 requestId = ++client->requestId;
    retval = UA_SecureChannel_sendAsymmetricOPNMessage(&client->channel, requestId, &opnSecRq,
                                                       &UA_TYPES[UA_TYPES_OPENSECURECHANNELREQUEST]);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_SECURECHANNEL,
                     "Sending OPN message failed with error %s", UA_StatusCode_name(retval));
        UA_Client_disconnect(client);
        return retval;
    }

    UA_LOG_DEBUG(&client->config.logger, UA_LOGCATEGORY_SECURECHANNEL, "OPN message sent");

    /* Increase nextChannelRenewal to avoid that we re-start renewal when
     * publish responses are received before the OPN response arrives. */
    client->nextChannelRenewal = UA_DateTime_nowMonotonic() +
        (2 * ((UA_DateTime)client->config.timeout * UA_DATETIME_MSEC));

    UA_DateTime now = UA_DateTime_nowMonotonic();
    UA_DateTime maxDate =  now + (client->config.timeout * UA_DATETIME_MSEC);

    /* Receive the OPN response */
    do {
        if(maxDate < UA_DateTime_nowMonotonic())
            return UA_STATUSCODE_BADCONNECTIONCLOSED;
        retval = receiveServiceResponse(client, NULL, NULL, maxDate, NULL);
    } while(client->state < UA_CLIENTSTATE_SECURECHANNEL && retval == UA_STATUSCODE_GOOD);

    return retval;
}

/* Gets a list of endpoints. Memory is allocated for endpointDescription array */
UA_StatusCode
UA_Client_getEndpointsInternal(UA_Client *client, const UA_String endpointUrl,
                               size_t *endpointDescriptionsSize,
                               UA_EndpointDescription **endpointDescriptions) {
    UA_GetEndpointsRequest request;
    UA_GetEndpointsRequest_init(&request);
    request.requestHeader.timestamp = UA_DateTime_now();
    request.requestHeader.timeoutHint = 10000;
    // assume the endpointurl outlives the service call
    request.endpointUrl = endpointUrl;

    UA_GetEndpointsResponse response;
    __UA_Client_Service(client, &request, &UA_TYPES[UA_TYPES_GETENDPOINTSREQUEST],
                        &response, &UA_TYPES[UA_TYPES_GETENDPOINTSRESPONSE]);

    if(response.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_StatusCode retval = response.responseHeader.serviceResult;
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "GetEndpointRequest failed with error code %s",
                     UA_StatusCode_name(retval));
        UA_GetEndpointsResponse_deleteMembers(&response);
        return retval;
    }
    *endpointDescriptions = response.endpoints;
    *endpointDescriptionsSize = response.endpointsSize;
    response.endpoints = NULL;
    response.endpointsSize = 0;
    UA_GetEndpointsResponse_deleteMembers(&response);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
selectEndpoint(UA_Client *client, const UA_String endpointUrl) {
    UA_EndpointDescription* endpointArray = NULL;
    size_t endpointArraySize = 0;
    UA_StatusCode retval =
        UA_Client_getEndpointsInternal(client, endpointUrl,
                                       &endpointArraySize, &endpointArray);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    UA_Boolean endpointFound = false;
    UA_Boolean tokenFound = false;
    UA_String binaryTransport = UA_STRING("http://opcfoundation.org/UA-Profile/"
                                          "Transport/uatcp-uasc-uabinary");

    UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Found %lu endpoints", (long unsigned)endpointArraySize);
    for(size_t i = 0; i < endpointArraySize; ++i) {
        UA_EndpointDescription* endpoint = &endpointArray[i];
        /* Match Binary TransportProfile?
         * Note: Siemens returns empty ProfileUrl, we will accept it as binary */
        if(endpoint->transportProfileUri.length != 0 &&
           !UA_String_equal(&endpoint->transportProfileUri, &binaryTransport))
            continue;

        /* Valid SecurityMode? */
        if(endpoint->securityMode < 1 || endpoint->securityMode > 3) {
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting endpoint %lu: invalid security mode", (long unsigned)i);
            continue;
        }

        /* Selected SecurityMode? */
        if(client->config.securityMode > 0 &&
           client->config.securityMode != endpoint->securityMode) {
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting endpoint %lu: security mode doesn't match", (long unsigned)i);
            continue;
        }

        /* Matching SecurityPolicy? */
        if(client->config.securityPolicyUri.length > 0 &&
           !UA_String_equal(&client->config.securityPolicyUri,
                            &endpoint->securityPolicyUri)) {
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting endpoint %lu: security policy doesn't match", (long unsigned)i);
            continue;
        }

        /* SecurityPolicy available? */
        if(!getSecurityPolicy(client, endpoint->securityPolicyUri)) {
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting endpoint %lu: security policy not available", (long unsigned)i);
            continue;
        }

        endpointFound = true;

        /* Select a matching UserTokenPolicy inside the endpoint */
        UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Endpoint %lu has %lu user token policies", (long unsigned)i, (long unsigned)endpoint->userIdentityTokensSize);
        for(size_t j = 0; j < endpoint->userIdentityTokensSize; ++j) {
            UA_UserTokenPolicy* userToken = &endpoint->userIdentityTokens[j];

            /* Usertokens also have a security policy... */
            if (userToken->securityPolicyUri.length > 0 &&
                !getSecurityPolicy(client, userToken->securityPolicyUri)) {
                UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting UserTokenPolicy %lu in endpoint %lu: security policy '%.*s' not available",
                (long unsigned)j, (long unsigned)i,
                (int)userToken->securityPolicyUri.length, userToken->securityPolicyUri.data);
                continue;
            }

            if(userToken->tokenType > 3) {
                UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting UserTokenPolicy %lu in endpoint %lu: invalid token type", (long unsigned)j, (long unsigned)i);
                continue;
            }

            /* Does the token type match the client configuration? */
            if (userToken->tokenType == UA_USERTOKENTYPE_ANONYMOUS &&
                client->config.userIdentityToken.content.decoded.type != &UA_TYPES[UA_TYPES_ANONYMOUSIDENTITYTOKEN] &&
                client->config.userIdentityToken.content.decoded.type != NULL) {
                UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting UserTokenPolicy %lu (anonymous) in endpoint %lu: configuration doesn't match", (long unsigned)j, (long unsigned)i);
                continue;
            }
            if (userToken->tokenType == UA_USERTOKENTYPE_USERNAME &&
                client->config.userIdentityToken.content.decoded.type != &UA_TYPES[UA_TYPES_USERNAMEIDENTITYTOKEN]) {
                UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting UserTokenPolicy %lu (username) in endpoint %lu: configuration doesn't match", (long unsigned)j, (long unsigned)i);
                continue;
            }
            if (userToken->tokenType == UA_USERTOKENTYPE_CERTIFICATE &&
                client->config.userIdentityToken.content.decoded.type != &UA_TYPES[UA_TYPES_X509IDENTITYTOKEN]) {
                UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting UserTokenPolicy %lu (certificate) in endpoint %lu: configuration doesn't match", (long unsigned)j, (long unsigned)i);
                continue;
            }
            if (userToken->tokenType == UA_USERTOKENTYPE_ISSUEDTOKEN &&
                client->config.userIdentityToken.content.decoded.type != &UA_TYPES[UA_TYPES_ISSUEDIDENTITYTOKEN]) {
                UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT, "Rejecting UserTokenPolicy %lu (token) in endpoint %lu: configuration doesn't match", (long unsigned)j, (long unsigned)i);
                continue;
            }

            /* Endpoint with matching UserTokenPolicy found. Copy to the configuration. */
            tokenFound = true;
            UA_EndpointDescription_deleteMembers(&client->config.endpoint);
            UA_EndpointDescription temp = *endpoint;
            temp.userIdentityTokensSize = 0;
            temp.userIdentityTokens = NULL;
            UA_UserTokenPolicy_deleteMembers(&client->config.userTokenPolicy);

            retval = UA_EndpointDescription_copy(&temp, &client->config.endpoint);
            if(retval != UA_STATUSCODE_GOOD) {
                UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                    "Copying endpoint description failed with error code %s",
                    UA_StatusCode_name(retval));
                break;
            }

            retval = UA_UserTokenPolicy_copy(userToken, &client->config.userTokenPolicy);
            if(retval != UA_STATUSCODE_GOOD) {
                UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                    "Copying user token policy failed with error code %s",
                    UA_StatusCode_name(retval));
                break;
            }

#if UA_LOGLEVEL <= 300
            const char *securityModeNames[3] = {"None", "Sign", "SignAndEncrypt"};
            const char *userTokenTypeNames[4] = {"Anonymous", "UserName",
                                                 "Certificate", "IssuedToken"};
            UA_String *securityPolicyUri = &userToken->securityPolicyUri;
            if(securityPolicyUri->length == 0)
                securityPolicyUri = &endpoint->securityPolicyUri;

            /* Log the selected endpoint */
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                        "Selected Endpoint %.*s with SecurityMode %s and SecurityPolicy %.*s",
                        (int)endpoint->endpointUrl.length, endpoint->endpointUrl.data,
                        securityModeNames[endpoint->securityMode - 1],
                        (int)endpoint->securityPolicyUri.length,
                        endpoint->securityPolicyUri.data);

            /* Log the selected UserTokenPolicy */
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                        "Selected UserTokenPolicy %.*s with UserTokenType %s and SecurityPolicy %.*s",
                        (int)userToken->policyId.length, userToken->policyId.data,
                        userTokenTypeNames[userToken->tokenType],
                        (int)securityPolicyUri->length, securityPolicyUri->data);
#endif
            break;
        }

        if(tokenFound)
            break;
    }

    UA_Array_delete(endpointArray, endpointArraySize,
                    &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);

    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    if(!endpointFound) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "No suitable endpoint found");
        retval = UA_STATUSCODE_BADINTERNALERROR;
    } else if(!tokenFound) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "No suitable UserTokenPolicy found for the possible endpoints");
        retval = UA_STATUSCODE_BADINTERNALERROR;
    }
    return retval;
}

UA_StatusCode
UA_Client_connectTCPSecureChannel(UA_Client *client, const UA_String endpointUrl) {
    if(client->state >= UA_CLIENTSTATE_CONNECTED)
        return UA_STATUSCODE_GOOD;

    UA_ChannelSecurityToken_init(&client->channel.securityToken);
    client->channel.state = UA_SECURECHANNELSTATE_FRESH;
    client->channel.sendSequenceNumber = 0;
    client->requestId = 0;
    client->channel.config = client->config.localConnectionConfig;

    /* Set the channel SecurityMode */
    client->channel.securityMode = client->config.endpoint.securityMode;
    if(client->channel.securityMode == UA_MESSAGESECURITYMODE_INVALID)
        client->channel.securityMode = UA_MESSAGESECURITYMODE_NONE;

    /* Initialized the SecureChannel */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_LOG_DEBUG(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                 "Initialize the SecurityPolicy context");
    if(!client->channel.securityPolicy) {
        /* Set the channel SecurityPolicy to #None if no endpoint is selected */
        UA_String sps = client->config.endpoint.securityPolicyUri;
        if(sps.length == 0) {
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                        "SecurityPolicy not specified -> use default #None");
            sps = UA_STRING("http://opcfoundation.org/UA/SecurityPolicy#None");
        }

        UA_SecurityPolicy *sp = getSecurityPolicy(client, sps);
        if(!sp) {
            UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                         "Failed to find the required security policy");
            retval = UA_STATUSCODE_BADINTERNALERROR;
            goto cleanup;
        }
        
        
        retval = UA_SecureChannel_setSecurityPolicy(&client->channel, sp,
                                                    &client->config.endpoint.serverCertificate);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                        "Failed to set the security policy");
            goto cleanup;
        }
    }

    /* Open a TCP connection */
    client->connection = client->config.connectionFunc(client->config.localConnectionConfig,
                                                       endpointUrl, client->config.timeout,
                                                       &client->config.logger);
    if(client->connection.state != UA_CONNECTION_OPENING) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "Opening the TCP socket failed");
        retval = UA_STATUSCODE_BADCONNECTIONCLOSED;
        goto cleanup;
    }

    UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                "TCP connection established");

    UA_Connection_attachSecureChannel(&client->connection, &client->channel);

    /* Perform the HEL/ACK handshake */
    client->channel.config = client->config.localConnectionConfig;
    retval = HelAckHandshake(client, endpointUrl);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "HEL/ACK handshake failed");
        goto cleanup;
    }
    setClientState(client, UA_CLIENTSTATE_CONNECTED);

    /* Open a SecureChannel. */
    client->channel.connection = &client->connection;
    retval = openSecureChannel(client, false);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "Opening a secure channel failed");
        goto cleanup;
    }

    return retval;

cleanup:
    UA_Client_disconnect(client);
    return retval;
}

UA_StatusCode
UA_Client_connectSession(UA_Client *client) {
    if(client->state < UA_CLIENTSTATE_SECURECHANNEL)
        return UA_STATUSCODE_BADINTERNALERROR;

    // TODO: actually, reactivate an existing session is working, but currently
    // republish is not implemented This option is disabled until we have a good
    // implementation of the subscription recovery.
    
    /* If we have the AuthenticationToken for a session, try ActivateSession.
     * Otherwise create a new Session. The async callback of the CreateSession
     * request will launch the ActivateSession request internally. */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(UA_NodeId_isNull(&client->authenticationToken)) {
        retval = createSessionAsync(client);
    } else {
        retval = activateSessionAsync(client);
    }
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Iterate until the Session is activated or we have a timeout */

    UA_DateTime nowTime = UA_DateTime_nowMonotonic();
    UA_DateTime maxTime = 
        nowTime + ((UA_DateTime)client->config.timeout * UA_DATETIME_MSEC);

    while(client->state != UA_CLIENTSTATE_SESSION) {
        if(maxTime < nowTime) {
            retval = UA_STATUSCODE_BADTIMEOUT;
            break;
        }
            
        UA_UInt16 timeout = (UA_UInt16)((maxTime - nowTime) / UA_DATETIME_MSEC);
        retval = UA_Client_run_iterate(client, timeout);
        if(retval != UA_STATUSCODE_GOOD)
            break;

        retval = client->connectStatus;
        if(retval != UA_STATUSCODE_GOOD)
            break;

        nowTime = UA_DateTime_nowMonotonic();
    }
    return retval;
}

#ifdef UA_ENABLE_ENCRYPTION
/* The local ApplicationURI has to match the certificates of the
 * SecurityPolicies */
static void
verifyClientApplicationURI(const UA_Client *client) {
#if UA_LOGLEVEL <= 400
    for(size_t i = 0; i < client->config.securityPoliciesSize; i++) {
        UA_SecurityPolicy *sp = &client->config.securityPolicies[i];
        UA_StatusCode retval =
            client->config.certificateVerification.
            verifyApplicationURI(client->config.certificateVerification.context,
                                 &sp->localCertificate,
                                 &client->config.clientDescription.applicationUri);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_WARNING(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                           "The configured ApplicationURI does not match the URI "
                           "specified in the certificate for the SecurityPolicy %.*s",
                           (int)sp->policyUri.length, sp->policyUri.data);
        }
    }
#endif
}
#endif

UA_Boolean
endpointUnconfigured(UA_Client *client) {
    UA_Byte test = 0;
    UA_Byte *pos = (UA_Byte*)&client->config.endpoint;
    for(size_t i = 0; i < sizeof(UA_EndpointDescription); i++)
        test = test | pos[i];
    pos = (UA_Byte*)&client->config.userTokenPolicy;
    for(size_t i = 0; i < sizeof(UA_UserTokenPolicy); i++)
        test = test | pos[i];
    return (test == 0);
}

UA_StatusCode
UA_Client_connectInternal(UA_Client *client, const UA_String endpointUrl) {
    if(client->state >= UA_CLIENTSTATE_CONNECTED)
        return UA_STATUSCODE_GOOD;

    UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                "Connecting to endpoint %.*s", (int)endpointUrl.length,
                endpointUrl.data);

#ifdef UA_ENABLE_ENCRYPTION
    verifyClientApplicationURI(client);
#endif

    /* Get endpoints only if the description has not been touched (memset to zero) */
    UA_Boolean getEndpoints = endpointUnconfigured(client);

    /* Connect up to the SecureChannel */
    UA_StatusCode retval = UA_Client_connectTCPSecureChannel(client, endpointUrl);
    if (retval != UA_STATUSCODE_GOOD) {
        UA_LOG_ERROR(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                     "Couldn't connect the client to a TCP secure channel");
        goto cleanup;
    }
    
    /* Get and select endpoints if required */
    if(getEndpoints) {
        UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                    "Endpoint and UserTokenPolicy unconfigured, perform GetEndpoints");
        retval = selectEndpoint(client, endpointUrl);
        if(retval != UA_STATUSCODE_GOOD)
            goto cleanup;

        /* Reconnect with a new SecureChannel if the current one does not match
         * the selected endpoint */
        if(!UA_String_equal(&client->config.endpoint.securityPolicyUri,
                            &client->channel.securityPolicy->policyUri)) {
            UA_LOG_INFO(&client->config.logger, UA_LOGCATEGORY_CLIENT,
                        "Disconnect to switch to a different SecurityPolicy");
            UA_Client_disconnect(client);
            return UA_Client_connectInternal(client, endpointUrl);
        }
    }

    retval = UA_Client_connectSession(client);
    if(retval != UA_STATUSCODE_GOOD)
        goto cleanup;

    return retval;

cleanup:
    UA_Client_disconnect(client);
    return retval;
}

UA_StatusCode
UA_Client_connect(UA_Client *client, const char *endpointUrl) {
    return UA_Client_connectInternal(client, UA_STRING((char*)(uintptr_t)endpointUrl));
}

UA_StatusCode
UA_Client_connect_noSession(UA_Client *client, const char *endpointUrl) {
    return UA_Client_connectTCPSecureChannel(client, UA_STRING((char*)(uintptr_t)endpointUrl));
}

UA_StatusCode
UA_Client_connect_username(UA_Client *client, const char *endpointUrl,
                           const char *username, const char *password) {
    UA_UserNameIdentityToken* identityToken = UA_UserNameIdentityToken_new();
    if(!identityToken)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    identityToken->userName = UA_STRING_ALLOC(username);
    identityToken->password = UA_STRING_ALLOC(password);
    UA_ExtensionObject_deleteMembers(&client->config.userIdentityToken);
    client->config.userIdentityToken.encoding = UA_EXTENSIONOBJECT_DECODED;
    client->config.userIdentityToken.content.decoded.type = &UA_TYPES[UA_TYPES_USERNAMEIDENTITYTOKEN];
    client->config.userIdentityToken.content.decoded.data = identityToken;
    return UA_Client_connect(client, endpointUrl);
}

/************************/
/* Close the Connection */
/************************/

static void
sendCloseSession(UA_Client *client) {
    UA_CloseSessionRequest request;
    UA_CloseSessionRequest_init(&request);

    request.requestHeader.timestamp = UA_DateTime_now();
    request.requestHeader.timeoutHint = 10000;
    request.deleteSubscriptions = true;
    UA_CloseSessionResponse response;
    __UA_Client_Service(client, &request, &UA_TYPES[UA_TYPES_CLOSESESSIONREQUEST],
                        &response, &UA_TYPES[UA_TYPES_CLOSESESSIONRESPONSE]);
    UA_CloseSessionRequest_deleteMembers(&request);
    UA_CloseSessionResponse_deleteMembers(&response);
}

static void
sendCloseSecureChannel(UA_Client *client) {
    UA_SecureChannel *channel = &client->channel;
    UA_CloseSecureChannelRequest request;
    UA_CloseSecureChannelRequest_init(&request);
    request.requestHeader.requestHandle = ++client->requestHandle;
    request.requestHeader.timestamp = UA_DateTime_now();
    request.requestHeader.timeoutHint = 10000;
    request.requestHeader.authenticationToken = client->authenticationToken;
    UA_SecureChannel_sendSymmetricMessage(channel, ++client->requestId,
                                          UA_MESSAGETYPE_CLO, &request,
                                          &UA_TYPES[UA_TYPES_CLOSESECURECHANNELREQUEST]);
    UA_CloseSecureChannelRequest_deleteMembers(&request);
    UA_SecureChannel_close(&client->channel);
    UA_SecureChannel_deleteMembers(&client->channel);
}

UA_StatusCode
UA_Client_disconnect(UA_Client *client) {
    /* Is a session established? */
    if(client->state >= UA_CLIENTSTATE_SESSION) {
        client->state = UA_CLIENTSTATE_SECURECHANNEL;
        sendCloseSession(client);
    }
    UA_NodeId_deleteMembers(&client->authenticationToken);
    client->requestHandle = 0;

    /* Is a secure channel established? */
    if(client->state >= UA_CLIENTSTATE_SECURECHANNEL) {
        client->state = UA_CLIENTSTATE_CONNECTED;
        sendCloseSecureChannel(client);
    }

    /* Close the TCP connection */
    if(client->connection.state != UA_CONNECTION_CLOSED
            && client->connection.state != UA_CONNECTION_OPENING)
        /* UA_ClientConnectionTCP_init sets initial state to opening */
        if(client->connection.close != NULL)
            client->connection.close(&client->connection);

#ifdef UA_ENABLE_SUBSCRIPTIONS
    // TODO REMOVE WHEN UA_SESSION_RECOVERY IS READY
    /* We need to clean up the subscriptions */
    UA_Client_Subscriptions_clean(client);
#endif

    /* Delete outstanding async services */
    UA_Client_AsyncService_removeAll(client, UA_STATUSCODE_BADSHUTDOWN);

    UA_SecureChannel_deleteMembers(&client->channel);

    setClientState(client, UA_CLIENTSTATE_DISCONNECTED);
    return UA_STATUSCODE_GOOD;
}
