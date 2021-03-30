/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2014-2020 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2014, 2016-2017 (c) Florian Palm
 *    Copyright 2015-2016 (c) Sten Grüner
 *    Copyright 2015 (c) Oleksiy Vasylyev
 *    Copyright 2016 (c) TorbenD
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2017-2018 (c) Mark Giraud, Fraunhofer IOSB
 */

#include <open62541/types_generated_encoding_binary.h>
#include <open62541/transport_generated_encoding_binary.h>
#include <open62541/transport_generated_handling.h>

#include "ua_securechannel.h"
#include "ua_util_internal.h"

UA_StatusCode
UA_SecureChannel_generateLocalNonce(UA_SecureChannel *channel) {
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    if(!sp)
        return UA_STATUSCODE_BADINTERNALERROR;

    UA_LOG_DEBUG_CHANNEL(sp->logger, channel, "Generating new local nonce");

    /* Is the length of the previous nonce correct? */
    size_t nonceLength = sp->symmetricModule.secureChannelNonceLength;
    if(channel->localNonce.length != nonceLength) {
        UA_ByteString_clear(&channel->localNonce);
        UA_StatusCode retval = UA_ByteString_allocBuffer(&channel->localNonce, nonceLength);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
    }

    return sp->symmetricModule.generateNonce(sp, &channel->localNonce);
}

UA_StatusCode
UA_SecureChannel_generateLocalKeys(const UA_SecureChannel *channel) {
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    if(!sp)
        return UA_STATUSCODE_BADINTERNALERROR;

    UA_LOG_TRACE_CHANNEL(sp->logger, channel, "Generating new local keys");

    void *cc = channel->channelContext;
    const UA_SecurityPolicyChannelModule *cm = &sp->channelModule;
    const UA_SecurityPolicySymmetricModule *sm = &sp->symmetricModule;
    const UA_SecurityPolicyCryptoModule *crm = &sm->cryptoModule;

    /* Generate symmetric key buffer of the required length. The block size is
     * identical for local/remote. */
    UA_ByteString buf;
    size_t encrKL = crm->encryptionAlgorithm.getLocalKeyLength(cc);
    size_t encrBS = crm->encryptionAlgorithm.getRemoteBlockSize(cc);
    size_t signKL = crm->signatureAlgorithm.getLocalKeyLength(cc);
    UA_StatusCode retval = UA_ByteString_allocBuffer(&buf, encrBS + signKL + encrKL);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    // No keys to generate
    if(buf.length == 0)
        return UA_STATUSCODE_GOOD;

    /* Generate key */
    retval = sm->generateKey(sp, &channel->remoteNonce, &channel->localNonce, &buf);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_ByteString_clear(&buf);
        return retval;
    }

    /* Set the channel context */
    const UA_ByteString localSigningKey = {signKL, buf.data};
    const UA_ByteString localEncryptingKey = {encrKL, &buf.data[signKL]};
    const UA_ByteString localIv = {encrBS, &buf.data[signKL + encrKL]};
    retval |= cm->setLocalSymSigningKey(cc, &localSigningKey);
    retval |= cm->setLocalSymEncryptingKey(cc, &localEncryptingKey);
    retval |= cm->setLocalSymIv(cc, &localIv);
    UA_ByteString_clear(&buf);
    return retval;
}

UA_StatusCode
generateRemoteKeys(const UA_SecureChannel *channel) {
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    if(!sp)
        return UA_STATUSCODE_BADINTERNALERROR;

    UA_LOG_TRACE_CHANNEL(sp->logger, channel, "Generating new remote keys");

    void *cc = channel->channelContext;
    const UA_SecurityPolicyChannelModule *cm = &sp->channelModule;
    const UA_SecurityPolicySymmetricModule *sm = &sp->symmetricModule;
    const UA_SecurityPolicyCryptoModule *crm = &sm->cryptoModule;

    /* Generate symmetric key buffer of the required length */
    UA_ByteString buf;
    size_t encrKL = crm->encryptionAlgorithm.getRemoteKeyLength(cc);
    size_t encrBS = crm->encryptionAlgorithm.getRemoteBlockSize(cc);
    size_t signKL = crm->signatureAlgorithm.getRemoteKeyLength(cc);
    UA_StatusCode retval = UA_ByteString_allocBuffer(&buf, encrBS + signKL + encrKL);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_ByteString_clear(&buf);
        return retval;
    }

    // No keys to generate.
    if(buf.length == 0)
        return UA_STATUSCODE_GOOD;

    /* Generate key */
    retval = sm->generateKey(sp, &channel->localNonce, &channel->remoteNonce, &buf);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Set the channel context */
    const UA_ByteString remoteSigningKey = {signKL, buf.data};
    const UA_ByteString remoteEncryptingKey = {encrKL, &buf.data[signKL]};
    const UA_ByteString remoteIv = {encrBS, &buf.data[signKL + encrKL]};
    retval |= cm->setRemoteSymSigningKey(cc, &remoteSigningKey);
    retval |= cm->setRemoteSymEncryptingKey(cc, &remoteEncryptingKey);
    retval |= cm->setRemoteSymIv(cc, &remoteIv);
    UA_ByteString_clear(&buf);
    return retval;
}

/***************************/
/* Send Asymmetric Message */
/***************************/

/* The length of the static header content */
#define UA_SECURECHANNEL_ASYMMETRIC_SECURITYHEADER_FIXED_LENGTH 12

size_t
calculateAsymAlgSecurityHeaderLength(const UA_SecureChannel *channel) {
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    if(!sp)
        return UA_STATUSCODE_BADINTERNALERROR;

    size_t asymHeaderLength = UA_SECURECHANNEL_ASYMMETRIC_SECURITYHEADER_FIXED_LENGTH +
                              sp->policyUri.length;
    if(channel->securityMode == UA_MESSAGESECURITYMODE_NONE)
        return asymHeaderLength;

    /* OPN is always encrypted even if the mode is sign only */
    asymHeaderLength += 20; /* Thumbprints are always 20 byte long */
    asymHeaderLength += sp->localCertificate.length;
    return asymHeaderLength;
}

UA_StatusCode
prependHeadersAsym(UA_SecureChannel *const channel, UA_Byte *header_pos,
                   const UA_Byte *buf_end, size_t totalLength,
                   size_t securityHeaderLength, UA_UInt32 requestId,
                   size_t *const encryptedLength) {
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    if(!sp)
        return UA_STATUSCODE_BADINTERNALERROR;

    if(channel->securityMode == UA_MESSAGESECURITYMODE_NONE) {
        *encryptedLength = totalLength;
    } else {
        size_t dataToEncryptLength = totalLength -
            (UA_SECURECHANNEL_CHANNELHEADER_LENGTH + securityHeaderLength);
        size_t plainTextBlockSize = sp->asymmetricModule.cryptoModule.
            encryptionAlgorithm.getRemotePlainTextBlockSize(channel->channelContext);
        size_t encryptedBlockSize = sp->asymmetricModule.cryptoModule.
            encryptionAlgorithm.getRemoteBlockSize(channel->channelContext);

        /* Padding always fills up the last block */
        UA_assert(dataToEncryptLength % plainTextBlockSize == 0);
        size_t blocks = dataToEncryptLength / plainTextBlockSize;
        *encryptedLength = totalLength + blocks * (encryptedBlockSize - plainTextBlockSize);
    }

    UA_TcpMessageHeader messageHeader;
    messageHeader.messageTypeAndChunkType = UA_MESSAGETYPE_OPN + UA_CHUNKTYPE_FINAL;
    messageHeader.messageSize = (UA_UInt32)*encryptedLength;
    UA_UInt32 secureChannelId = channel->securityToken.channelId;
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    retval |= UA_encodeBinary(&messageHeader, &UA_TRANSPORT[UA_TRANSPORT_TCPMESSAGEHEADER],
                              &header_pos, &buf_end, NULL, NULL);
    retval |= UA_encodeBinary(&secureChannelId, &UA_TYPES[UA_TYPES_UINT32],
                              &header_pos, &buf_end, NULL, NULL);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    UA_AsymmetricAlgorithmSecurityHeader asymHeader;
    UA_AsymmetricAlgorithmSecurityHeader_init(&asymHeader);
    asymHeader.securityPolicyUri = sp->policyUri;
    if(channel->securityMode == UA_MESSAGESECURITYMODE_SIGN ||
       channel->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT) {
        asymHeader.senderCertificate = sp->localCertificate;
        asymHeader.receiverCertificateThumbprint.length = 20;
        asymHeader.receiverCertificateThumbprint.data = channel->remoteCertificateThumbprint;
    }
    retval = UA_encodeBinary(&asymHeader,
                             &UA_TRANSPORT[UA_TRANSPORT_ASYMMETRICALGORITHMSECURITYHEADER],
                             &header_pos, &buf_end, NULL, NULL);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    UA_SequenceHeader seqHeader;
    seqHeader.requestId = requestId;
    seqHeader.sequenceNumber = UA_atomic_addUInt32(&channel->sendSequenceNumber, 1);
    retval = UA_encodeBinary(&seqHeader, &UA_TRANSPORT[UA_TRANSPORT_SEQUENCEHEADER],
                             &header_pos, &buf_end, NULL, NULL);
    return retval;
}

void
hideBytesAsym(const UA_SecureChannel *channel, UA_Byte **buf_start,
              const UA_Byte **buf_end) {
    /* Set buf_start to the beginning of the payload body */
    *buf_start += UA_SECURECHANNEL_CHANNELHEADER_LENGTH;
    *buf_start += calculateAsymAlgSecurityHeaderLength(channel);
    *buf_start += UA_SECURECHANNEL_SEQUENCEHEADER_LENGTH;

#ifdef UA_ENABLE_ENCRYPTION
    if(channel->securityMode == UA_MESSAGESECURITYMODE_NONE)
        return;

    /* Block sizes depend on the remote key (certificate) */
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    size_t signatureSize = sp->asymmetricModule.cryptoModule.signatureAlgorithm.
        getLocalSignatureSize(channel->channelContext);
    size_t plainTextBlockSize = sp->asymmetricModule.cryptoModule.
        encryptionAlgorithm.getRemotePlainTextBlockSize(channel->channelContext);
    size_t encryptedBlockSize = sp->asymmetricModule.cryptoModule.
        encryptionAlgorithm.getRemoteBlockSize(channel->channelContext);
    UA_Boolean extraPadding = (sp->asymmetricModule.cryptoModule.encryptionAlgorithm.
                               getRemoteKeyLength(channel->channelContext) > 2048);

    /* Encrypted plaintext is sequence header + body + padding + signature.
     * Set buf_end to the worst-case lower bound for the payload body end. */
    size_t plainTextMaxLength = (size_t)(*buf_end - *buf_start) +
        UA_SECURECHANNEL_SEQUENCEHEADER_LENGTH;
    size_t max_blocks = plainTextMaxLength / plainTextBlockSize;
    size_t max_overhead = max_blocks * (encryptedBlockSize - plainTextBlockSize);
    size_t paddingBytes = (UA_LIKELY(!extraPadding)) ? 1u : 2u;
    *buf_end -= max_overhead + paddingBytes + signatureSize;
#endif
}

#ifdef UA_ENABLE_ENCRYPTION

/* Assumes that pos can be advanced to the end of the current block */
void
padChunk(UA_SecureChannel *channel, const UA_SecurityPolicyCryptoModule *cm,
         const UA_Byte *start, UA_Byte **pos) {
    const size_t bytesToWrite = (uintptr_t)*pos - (uintptr_t)start;
    size_t signatureSize = cm->signatureAlgorithm.
        getLocalSignatureSize(channel->channelContext);
    size_t plainTextBlockSize = cm->encryptionAlgorithm.
        getRemotePlainTextBlockSize(channel->channelContext);
    UA_Boolean extraPadding = (cm->encryptionAlgorithm.
                               getRemoteKeyLength(channel->channelContext) > 2048);
    size_t paddingBytes = (UA_LIKELY(!extraPadding)) ? 1u : 2u;
    size_t paddingLength = plainTextBlockSize -
        ((bytesToWrite + signatureSize + paddingBytes) % plainTextBlockSize);

    /* Write the padding. This is <= because the paddingSize byte also has to be
     * written */
    UA_Byte paddingByte = (UA_Byte)paddingLength;
    for(UA_UInt16 i = 0; i <= paddingLength; ++i) {
        **pos = paddingByte;
        ++*pos;
    }

    /* Write the extra padding byte if required */
    if(extraPadding) {
        **pos = (UA_Byte)(paddingLength >> 8u);
        ++*pos;
    }
}

UA_StatusCode
signAndEncryptAsym(UA_SecureChannel *channel, size_t preSignLength,
                   UA_ByteString *buf, size_t securityHeaderLength,
                   size_t totalLength) {
    if(channel->securityMode != UA_MESSAGESECURITYMODE_SIGN &&
       channel->securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        return UA_STATUSCODE_GOOD;

    const UA_SecurityPolicy *sp = channel->securityPolicy;
    
    /* Sign message */
    const UA_ByteString dataToSign = {preSignLength, buf->data};
    size_t sigsize = sp->asymmetricModule.cryptoModule.signatureAlgorithm.
        getLocalSignatureSize(channel->channelContext);
    UA_ByteString signature = {sigsize, buf->data + preSignLength};
    UA_StatusCode retval = sp->asymmetricModule.cryptoModule.signatureAlgorithm.
        sign(channel->channelContext, &dataToSign, &signature);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Specification part 6, 6.7.4: The OpenSecureChannel Messages are
     * signed and encrypted if the SecurityMode is not None (even if the
     * SecurityMode is SignOnly). */
    size_t unencrypted_length =
        UA_SECURECHANNEL_CHANNELHEADER_LENGTH + securityHeaderLength;
    UA_ByteString dataToEncrypt = {totalLength - unencrypted_length,
                                   &buf->data[unencrypted_length]};
    return sp->asymmetricModule.cryptoModule.encryptionAlgorithm.
        encrypt(channel->channelContext, &dataToEncrypt);
}

/**************************/
/* Send Symmetric Message */
/**************************/

UA_StatusCode
signAndEncryptSym(UA_MessageContext *messageContext,
                  size_t preSigLength, size_t totalLength) {
    const UA_SecureChannel *channel = messageContext->channel;
    if(channel->securityMode == UA_MESSAGESECURITYMODE_NONE)
        return UA_STATUSCODE_GOOD;

    /* Sign */
    const UA_SecurityPolicy *sp = channel->securityPolicy;
    UA_ByteString dataToSign = messageContext->messageBuffer;
    dataToSign.length = preSigLength;
    UA_ByteString signature;
    signature.length = sp->symmetricModule.cryptoModule.signatureAlgorithm.
        getLocalSignatureSize(channel->channelContext);
    signature.data = messageContext->buf_pos;
    UA_StatusCode res = sp->symmetricModule.cryptoModule.signatureAlgorithm.
        sign(channel->channelContext, &dataToSign, &signature);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    if(channel->securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
        return UA_STATUSCODE_GOOD;

    /* Encrypt */
    UA_ByteString dataToEncrypt;
    dataToEncrypt.data = messageContext->messageBuffer.data +
        UA_SECURECHANNEL_CHANNELHEADER_LENGTH + 
        UA_SECURECHANNEL_SYMMETRIC_SECURITYHEADER_LENGTH;
    dataToEncrypt.length = totalLength -
        (UA_SECURECHANNEL_CHANNELHEADER_LENGTH + 
         UA_SECURECHANNEL_SYMMETRIC_SECURITYHEADER_LENGTH);
    return sp->symmetricModule.cryptoModule.encryptionAlgorithm.
        encrypt(channel->channelContext, &dataToEncrypt);
}

#endif /* UA_ENABLE_ENCRYPTION */

void
setBufPos(UA_MessageContext *mc) {
    /* Forward the data pointer so that the payload is encoded after the message
     * header. This has to be a symmetric message because OPN (with asymmetric
     * encryption) does not support chunking. */
    mc->buf_pos = &mc->messageBuffer.data[UA_SECURECHANNEL_SYMMETRIC_HEADER_TOTALLENGTH];
    mc->buf_end = &mc->messageBuffer.data[mc->messageBuffer.length];

#ifdef UA_ENABLE_ENCRYPTION
    if(mc->channel->securityMode == UA_MESSAGESECURITYMODE_NONE)
        return;

    const UA_SecureChannel *channel = mc->channel;
    const UA_SecurityPolicy *sp = channel->securityPolicy;

    /* Reserve space for the message footer at the end of the chunk if the chunk
     * is signed and/or encrypted. The footer includes the fields PaddingSize,
     * Padding, ExtraPadding and Signature. The padding fields are only present
     * if the chunk is encrypted. */
    mc->buf_end -= sp->symmetricModule.cryptoModule.signatureAlgorithm.
        getLocalSignatureSize(channel->channelContext);

    /* The size of the padding depends on the amount of data that shall be sent
     * and is unknown at this point. Reserve space for the PaddingSize byte,
     * the maximum amount of Padding which equals the block size of the
     * symmetric encryption algorithm and last 1 byte for the ExtraPaddingSize
     * field that is present if the encryption key is larger than 2048 bits.
     * The actual padding size is later calculated by the function
     * calculatePaddingSym(). */
    if(channel->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT) {
        /* PaddingSize and ExtraPaddingSize fields */
        UA_Boolean extraPadding =
            (sp->symmetricModule.cryptoModule.encryptionAlgorithm.
             getRemoteKeyLength(channel->channelContext) > 2048);
        mc->buf_end -= (UA_LIKELY(!extraPadding)) ? 1 : 2;

        /* Assuming that for symmetric encryption the plainTextBlockSize ==
         * cypherTextBlockSize. For symmetric encryption the remote/local block
         * sizes are identical. */
        UA_assert(sp->symmetricModule.cryptoModule.encryptionAlgorithm.
                  getRemotePlainTextBlockSize(channel->channelContext)
                  ==
                  sp->symmetricModule.cryptoModule.encryptionAlgorithm.
                  getRemoteBlockSize(channel->channelContext));
    }
#endif
}

/****************************/
/* Process a received Chunk */
/****************************/

static size_t
decodePadding(const UA_SecureChannel *channel,
              const UA_SecurityPolicyCryptoModule *cryptoModule,
              const UA_ByteString *chunk, size_t sigsize) {
    size_t paddingSize = chunk->data[chunk->length - sigsize - 1];
    size_t keyLength = cryptoModule->encryptionAlgorithm.
        getLocalKeyLength(channel->channelContext);
    if(keyLength > 2048) {
        /* Extra padding size */
        paddingSize <<= 8u;
        paddingSize += chunk->data[chunk->length - sigsize - 2];
        paddingSize += 1; /* Extra padding byte itself */
    }

    /* Add one since the paddingSize byte itself needs to be removed as well */
    paddingSize += 1;

    UA_LOG_TRACE_CHANNEL(channel->securityPolicy->logger, channel,
                         "Calculated padding size to be %lu",
                         (long unsigned int)paddingSize);
    return paddingSize;
}

static UA_StatusCode
verifySignature(const UA_SecureChannel *channel,
                const UA_SecurityPolicyCryptoModule *cryptoModule,
                const UA_ByteString *chunk, size_t sigsize) {
    UA_LOG_TRACE_CHANNEL(channel->securityPolicy->logger, channel,
                         "Verifying chunk signature");
    if(sigsize >= chunk->length)
        return UA_STATUSCODE_BADSECURITYCHECKSFAILED;
    const UA_ByteString content = {chunk->length - sigsize, chunk->data};
    const UA_ByteString sig = {sigsize, chunk->data + chunk->length - sigsize};
    UA_StatusCode retval = cryptoModule->signatureAlgorithm.
        verify(channel->channelContext, &content, &sig);
#ifdef UA_ENABLE_UNIT_TEST_FAILURE_HOOKS
    retval |= decrypt_verifySignatureFailure;
#endif
    return retval;
}

/* Sets the payload to a pointer inside the chunk buffer. Returns the requestId
 * and the sequenceNumber */
UA_StatusCode
decryptAndVerifyChunk(const UA_SecureChannel *channel,
                      const UA_SecurityPolicyCryptoModule *cryptoModule,
                      UA_MessageType messageType, UA_ByteString *chunk,
                      size_t offset) {
    /* Decrypt the chunk */
    UA_StatusCode res = UA_STATUSCODE_GOOD;
    if(channel->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT ||
       messageType == UA_MESSAGETYPE_OPN) {
        UA_ByteString cipher = {chunk->length - offset, chunk->data + offset};
        res = cryptoModule->encryptionAlgorithm.decrypt(channel->channelContext, &cipher);
        if(res != UA_STATUSCODE_GOOD)
            return res;
        chunk->length = cipher.length + offset;
    }

    /* Does the message have a signature? */
    if(channel->securityMode != UA_MESSAGESECURITYMODE_SIGN &&
       channel->securityMode != UA_MESSAGESECURITYMODE_SIGNANDENCRYPT &&
       messageType != UA_MESSAGETYPE_OPN)
        return UA_STATUSCODE_GOOD;

    /* Verify the chunk signature */
    size_t sigsize = cryptoModule->signatureAlgorithm.
        getRemoteSignatureSize(channel->channelContext);
    res = verifySignature(channel, cryptoModule, chunk, sigsize);
    if(res != UA_STATUSCODE_GOOD)
        return res;

    /* Compute the padding if the payload as encrypted */
    size_t padSize = 0;
    if(channel->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT ||
       (messageType == UA_MESSAGETYPE_OPN &&
        cryptoModule->encryptionAlgorithm.uri.length > 0))
        padSize = decodePadding(channel, cryptoModule, chunk, sigsize);

    /* Verify the content length. The encrypted payload has to be at least 9
     * bytes long: 8 byte for the SequenceHeader and one byte for the actual
     * message */
    if(offset + padSize + sigsize + 9 >= chunk->length)
        return UA_STATUSCODE_BADSECURITYCHECKSFAILED;

    /* Hide the signature and padding */
    chunk->length -= (sigsize + padSize);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
checkAsymHeader(UA_SecureChannel *channel,
                const UA_AsymmetricAlgorithmSecurityHeader *asymHeader) {
    const UA_SecurityPolicy *sp = channel->securityPolicy;

    if(!UA_ByteString_equal(&sp->policyUri, &asymHeader->securityPolicyUri))
        return UA_STATUSCODE_BADSECURITYPOLICYREJECTED;

    return sp->asymmetricModule.
        compareCertificateThumbprint(sp, &asymHeader->receiverCertificateThumbprint);

    /* The certificate in the header is verified via the configured PKI plugin
     * as certificateVerification.verifyCertificate(...). We cannot do it here
     * because the client/server context is needed. */
}

UA_StatusCode
checkSymHeader(UA_SecureChannel *channel, const UA_UInt32 tokenId) {
    /* If no match, try to revolve to the next token after a
     * RenewSecureChannel */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_ChannelSecurityToken *token = &channel->securityToken;
    switch(channel->renewState) {
    case UA_SECURECHANNELRENEWSTATE_NORMAL:
    case UA_SECURECHANNELRENEWSTATE_SENT:
    default:
        break;

    case UA_SECURECHANNELRENEWSTATE_NEWTOKEN_SERVER:
        /* Old token still in use */
        if(tokenId == channel->securityToken.tokenId)
            break;

        /* Not the new token */
        if(tokenId != channel->altSecurityToken.tokenId) {
            UA_LOG_WARNING_CHANNEL(channel->securityPolicy->logger, channel,
                                   "Unknown SecurityToken");
            return UA_STATUSCODE_BADSECURECHANNELTOKENUNKNOWN;
        }

        /* Roll over to the new token, generate new local and remote keys */
        channel->renewState = UA_SECURECHANNELRENEWSTATE_NORMAL;
        channel->securityToken = channel->altSecurityToken;
        UA_ChannelSecurityToken_init(&channel->altSecurityToken);
        retval = UA_SecureChannel_generateLocalKeys(channel);
        retval |= generateRemoteKeys(channel);
        break;

    case UA_SECURECHANNELRENEWSTATE_NEWTOKEN_CLIENT:
        /* The server is still using the old token. That's okay. */
        if(tokenId == channel->altSecurityToken.tokenId) {
            token = &channel->altSecurityToken;
            break;
        }

        /* Not the new token */
        if(tokenId != channel->securityToken.tokenId) {
            UA_LOG_WARNING_CHANNEL(channel->securityPolicy->logger, channel,
                                   "Unknown SecurityToken");
            return UA_STATUSCODE_BADSECURECHANNELTOKENUNKNOWN;
        }

        /* The remote server uses the new token for the first time. Delete the
         * old token and roll the remote key over. The local key already uses
         * the nonce pair from the last OPN exchange. */
        channel->renewState = UA_SECURECHANNELRENEWSTATE_NORMAL;
        UA_ChannelSecurityToken_init(&channel->altSecurityToken);
        retval = generateRemoteKeys(channel);
    }

    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    UA_DateTime timeout = token->createdAt + (token->revisedLifetime * UA_DATETIME_MSEC);
    if(channel->state == UA_SECURECHANNELSTATE_OPEN &&
       timeout < UA_DateTime_nowMonotonic()) {
        UA_LOG_WARNING_CHANNEL(channel->securityPolicy->logger, channel,
                               "SecurityToken timed out");
        UA_SecureChannel_close(channel);
        return UA_STATUSCODE_BADSECURECHANNELCLOSED;
    }

    return UA_STATUSCODE_GOOD;
}
