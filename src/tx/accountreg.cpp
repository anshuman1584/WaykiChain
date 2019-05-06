// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php


#include "accountreg.h"

#include "commons/serialize.h"
#include "tx.h"
#include "txdb.h"
#include "crypto/hash.h"
#include "util.h"
#include "database.h"
#include "main.h"
#include "vm/vmrunenv.h"
#include "core.h"
#include "miner/miner.h"
#include "version.h"


bool CAccountRegisterTx::CheckTx(CValidationState &state, CAccountViewCache &view,
                                CScriptDBViewCache &scriptDB) {
    if (txUid.type() != typeid(CPubKey))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, userId must be CPubKey"),
            REJECT_INVALID, "userid-type-error");

    if ((minerUid.type() != typeid(CPubKey)) && (minerUid.type() != typeid(CNullID)))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, minerId must be CPubKey or CNullID"),
            REJECT_INVALID, "minerid-type-error");

    if (!txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, register tx public key is invalid"),
            REJECT_INVALID, "bad-regtx-publickey");

    if (!CheckMoneyRange(llFees))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, register tx fee out of range"),
            REJECT_INVALID, "bad-regtx-fee-toolarge");

    if (!CheckMinTxFee(llFees)) {
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, register tx fee smaller than MinTxFee"),
            REJECT_INVALID, "bad-tx-fee-toosmall");
    }

    if (!CheckSignatureSize(signature)) {
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, signature size invalid"),
            REJECT_INVALID, "bad-tx-sig-size");
    }

    // check signature script
    uint256 sighash = SignatureHash();
    if (!CheckSignScript(sighash, signature, txUid.get<CPubKey>()))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::CheckTx, register tx signature error "),
            REJECT_INVALID, "bad-regtx-signature");

    return true;
}

bool CAccountRegisterTx::ExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
                                   CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache,
                                   CScriptDBViewCache &scriptDB) {
    CAccount account;
    CRegID regId(nHeight, nIndex);
    CKeyID keyId = txUid.get<CPubKey>().GetKeyId();
    if (!view.GetAccount(txUid, account))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::ExecuteTx, read source keyId %s account info error",
            keyId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    CAccountLog acctLog(account);
    if (account.pubKey.IsFullyValid() && account.pubKey.GetKeyId() == keyId)
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::ExecuteTx, read source keyId %s duplicate register",
            keyId.ToString()), UPDATE_ACCOUNT_FAIL, "duplicate-register-account");

    account.pubKey = txUid.get<CPubKey>();
    if (llFees > 0 && !account.OperateAccount(MINUS_FREE, llFees, nHeight)) {
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::ExecuteTx, not sufficient funds in account, keyid=%s",
                        keyId.ToString()), UPDATE_ACCOUNT_FAIL, "not-sufficiect-funds");
    }

    account.regID = regId;
    if (typeid(CPubKey) == minerUid.type()) {
        account.minerPubKey = minerUid.get<CPubKey>();
        if (account.minerPubKey.IsValid() && !account.minerPubKey.IsFullyValid()) {
            return state.DoS(100, ERRORMSG("CAccountRegisterTx::ExecuteTx, minerPubKey:%s Is Invalid",
                account.minerPubKey.ToString()), UPDATE_ACCOUNT_FAIL, "MinerPKey Is Invalid");
        }
    }

    if (!view.SaveAccountInfo(regId, keyId, account))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::ExecuteTx, write source addr %s account info error",
            regId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    txundo.vAccountLog.push_back(acctLog);
    txundo.txHash = GetHash();
    if(SysCfg().GetAddressToTxFlag()) {
        CScriptDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        if(!view.GetKeyId(txUid, sendKeyId))
            return ERRORMSG("CAccountRegisterTx::ExecuteTx, get keyid by userId error!");

        if(!scriptDB.SetTxHashByAddress(sendKeyId, nHeight, nIndex+1, txundo.txHash.GetHex(), operAddressToTxLog))
            return false;

        txundo.vScriptOperLog.push_back(operAddressToTxLog);
    }

    return true;
}

bool CAccountRegisterTx::UndoExecuteTx(int nIndex, CAccountViewCache &view, CValidationState &state,
        CTxUndo &txundo, int nHeight, CTransactionDBCache &txCache, CScriptDBViewCache &scriptDB) {
    // drop account
    CRegID accountId(nHeight, nIndex);
    CAccount oldAccount;
    if (!view.GetAccount(accountId, oldAccount))
        return state.DoS(100, ERRORMSG("CAccountRegisterTx::UndoExecuteTx, read secure account=%s info error",
            accountId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");

    CKeyID keyId;
    view.GetKeyId(accountId, keyId);

    if (llFees > 0) {
        CAccountLog accountLog;
        if (!txundo.GetAccountOperLog(keyId, accountLog))
            return state.DoS(100, ERRORMSG("CAccountRegisterTx::UndoExecuteTx, read keyId=%s tx undo info error", keyId.GetHex()),
                    UPDATE_ACCOUNT_FAIL, "bad-read-txundoinfo");
        oldAccount.UndoOperateAccount(accountLog);
    }

    if (!oldAccount.IsEmptyValue()) {
        CPubKey empPubKey;
        oldAccount.pubKey = empPubKey;
        oldAccount.minerPubKey = empPubKey;
        oldAccount.regID.Clean();
        CUserID userId(keyId);
        view.SetAccount(userId, oldAccount);
    } else {
        view.EraseAccount(txUid);
    }
    view.EraseId(accountId);
    return true;
}

bool CAccountRegisterTx::GetAddress(set<CKeyID> &vAddr, CAccountViewCache &view, CScriptDBViewCache &scriptDB) {
    if (!txUid.get<CPubKey>().IsFullyValid())
        return false;

    vAddr.insert(txUid.get<CPubKey>().GetKeyId());
    return true;
}

string CAccountRegisterTx::ToString(CAccountViewCache &view) const {
    string str;
    str += strprintf("txType=%s, hash=%s, ver=%d, pubkey=%s, llFees=%ld, keyid=%s, nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString().c_str(), nVersion, txUid.get<CPubKey>().ToString(),
        llFees, txUid.get<CPubKey>().GetKeyId().ToAddress(), nValidHeight);

    return str;
}

Object CAccountRegisterTx::ToJson(const CAccountViewCache &AccountView) const {
    assert(txUid.type() == typeid(CPubKey));
    string address = txUid.get<CPubKey>().GetKeyId().ToAddress();
    string userPubKey = txUid.ToString();
    string userMinerPubKey = minerUid.ToString();

    Object result;
    result.push_back(Pair("hash",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("addr",           address));
    result.push_back(Pair("pubkey",         userPubKey));
    result.push_back(Pair("miner_pubkey",   userMinerPubKey));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("valid_height",   nValidHeight));
    return result;
}
