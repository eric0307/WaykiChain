// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "contracttx.h"

#include "accounts/vote.h"
#include "commons/serialize.h"
#include "crypto/hash.h"
#include "main.h"
#include "miner/miner.h"
#include "persistence/contractdb.h"
#include "persistence/txdb.h"
#include "util.h"
#include "version.h"
#include "vm/vmrunenv.h"

static bool GetKeyId(const CAccountCache &view, const vector<unsigned char> &ret, CKeyID &KeyId) {
    if (ret.size() == 6) {
        CRegID regId(ret);
        KeyId = regId.GetKeyId(view);
    } else if (ret.size() == 34) {
        string addr(ret.begin(), ret.end());
        KeyId = CKeyID(addr);
    } else {
        return false;
    }

    if (KeyId.IsEmpty()) return false;

    return true;
}

bool CContractDeployTx::ExecuteTx(int nHeight, int nIndex, CCacheWrapper &cw, CValidationState &state) {
    CAccount acctInfo;
    CContractDBOperLog operLog;
    if (!cw.pAccountCache->GetAccount(txUid, acctInfo)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, read regist addr %s account info error",
                        txUid.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }

    CAccount acctInfoLog(acctInfo);
    if (!acctInfo.OperateBalance(CoinType::WICC, MINUS_VALUE, llFees)) {
            return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, operate account failed ,regId=%s",
                            txUid.ToString()), UPDATE_ACCOUNT_FAIL, "operate-account-failed");

        cw.pTxUndo->vAccountLog.push_back(acctInfoLog);
    }
    cw.pTxUndo->txHash = GetHash();

    CRegID regId(nHeight, nIndex);
    //create script account
    CKeyID keyId = Hash160(regId.GetRegIdRaw());
    CAccount account;
    account.keyID = keyId;
    account.regID = regId;
    account.nickID = CNickID();
    //save new script content
    if(!cw.pContractCache->SetScript(regId, contractScript)){
        return state.DoS(100,
            ERRORMSG("CContractDeployTx::ExecuteTx, save script id %s script info error",
                regId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }
    if (!cw.pAccountCache->SaveAccount(account)) {
        return state.DoS(100,
            ERRORMSG("CContractDeployTx::ExecuteTx, create new account script id %s script info error",
                regId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    nRunStep = contractScript.size();

    if(!operLog.vKey.empty()) {
        cw.pTxUndo->vContractOperLog.push_back(operLog);
    }
    CUserID userId = acctInfo.keyID;
    if (!cw.pAccountCache->SetAccount(userId, acctInfo))
        return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, save account info error"),
            UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");

    if(SysCfg().GetAddressToTxFlag()) {
        CContractDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        if(!cw.pAccountCache->GetKeyId(txUid, sendKeyId)) {
            return ERRORMSG("CContractDeployTx::ExecuteTx, get regAcctId by account error!");
        }

        if(!cw.pContractCache->SetTxHashByAddress(sendKeyId, nHeight, nIndex+1, cw.pTxUndo->txHash.GetHex(), operAddressToTxLog))
            return false;

        cw.pTxUndo->vContractOperLog.push_back(operAddressToTxLog);
    }
    return true;
}

bool CContractDeployTx::UndoExecuteTx(int nHeight, int nIndex, CCacheWrapper &cw, CValidationState &state) {
    CAccount account;
    CUserID userId;
    if (!cw.pAccountCache->GetAccount(txUid, account)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, read regist addr %s account info error",
                                        account.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }

    CRegID contractRegId(nHeight, nIndex);
    //delete script content
    if (!cw.pContractCache->EraseScript(contractRegId)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, erase script id %s error",
                                        contractRegId.ToString()), UPDATE_ACCOUNT_FAIL, "erase-script-failed");
    }
    //delete account
    if (!cw.pAccountCache->EraseKeyId(contractRegId)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, erase script account %s error", contractRegId.ToString()),
                         UPDATE_ACCOUNT_FAIL, "erase-appkeyid-failed");
    }
    CKeyID keyId = Hash160(contractRegId.GetRegIdRaw());
    if (!cw.pAccountCache->EraseAccountByKeyId(keyId)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, erase script account %s error", contractRegId.ToString()),
                         UPDATE_ACCOUNT_FAIL, "erase-appaccount-failed");
    }
    // LogPrint("INFO", "Delete regid %s app account\n", contractRegId.ToString());

    for (auto &itemLog : cw.pTxUndo->vAccountLog) {
        if (itemLog.keyID == account.keyID) {
            if (!account.UndoOperateAccount(itemLog))
                return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, undo operate account error, keyId=%s", account.keyID.ToString()),
                                 UPDATE_ACCOUNT_FAIL, "undo-account-failed");
        }
    }

    vector<CContractDBOperLog>::reverse_iterator rIterScriptDBLog = cw.pTxUndo->vContractOperLog.rbegin();
    for (; rIterScriptDBLog != cw.pTxUndo->vContractOperLog.rend(); ++rIterScriptDBLog) {
        if (!cw.pContractCache->UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, undo scriptdb data error"),
                                            UPDATE_ACCOUNT_FAIL, "undo-scriptdb-failed");
    }

    userId = account.keyID;
    if (!cw.pAccountCache->SetAccount(userId, account))
        return state.DoS(100, ERRORMSG("CContractDeployTx::UndoExecuteTx, save account error"),
                                        UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");

    return true;
}

bool CContractDeployTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    CKeyID keyId;
    if (!cw.pAccountCache->GetKeyId(txUid, keyId))
        return false;

    keyIds.insert(keyId);
    return true;
}

string CContractDeployTx::ToString(CAccountCache &view) {
    CKeyID keyId;
    view.GetKeyId(txUid, keyId);
    string str = strprintf("txType=%s, hash=%s, ver=%d, accountId=%s, keyid=%s, llFees=%ld, nValidHeight=%d\n",
                            GetTxType(nTxType), GetHash().ToString().c_str(), nVersion,
                            txUid.ToString(), keyId.GetHex(), llFees, nValidHeight);
    return str;
}

Object CContractDeployTx::ToJson(const CAccountCache &accountCache) const{
    Object result;
    CAccountCache view(accountCache);

    CKeyID keyid;
    view.GetKeyId(txUid, keyid);

    result.push_back(Pair("hash", GetHash().GetHex()));
    result.push_back(Pair("tx_type", GetTxType(nTxType)));
    result.push_back(Pair("ver", nVersion));
    result.push_back(Pair("regid", txUid.get<CRegID>().ToString()));
    result.push_back(Pair("addr", keyid.ToAddress()));
    result.push_back(Pair("script", "script_content"));
    result.push_back(Pair("fees", llFees));
    result.push_back(Pair("valid_height", nValidHeight));
    return result;
}

bool CContractDeployTx::CheckTx(CCacheWrapper &cw, CValidationState &state) {
    IMPLEMENT_CHECK_TX_FEE;
    IMPLEMENT_CHECK_TX_REGID(txUid.type());

    CDataStream stream(contractScript, SER_DISK, CLIENT_VERSION);
    CVmScript vmScript;
    try {
        stream >> vmScript;
    } catch (exception &e) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, unserialize to vmScript error"),
                         REJECT_INVALID, "unserialize-error");
    }
    if (!vmScript.IsValid()) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, vmScript is invalid"),
                         REJECT_INVALID, "vmscript-invalid");
    }
    uint64_t llFuel = ceil(contractScript.size() / 100) * GetFuelRate(*cw.pContractCache);
    if (llFuel < 1 * COIN) {
        llFuel = 1 * COIN;
    }
    if (llFees < llFuel) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, register app tx fee too litter "
                        "(actual:%lld vs need:%lld)", llFees, llFuel), REJECT_INVALID, "fee-too-litter");
    }

    CAccount acctInfo;
    if (!cw.pAccountCache->GetAccount(txUid, acctInfo)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, get account failed"),
                         REJECT_INVALID, "bad-getaccount");
    }
    if (!acctInfo.IsRegistered()) {
        return state.DoS(
            100, ERRORMSG("CContractDeployTx::CheckTx, account have not registered public key"),
            REJECT_INVALID, "bad-no-pubkey");
    }

    IMPLEMENT_CHECK_TX_SIGNATURE(acctInfo.pubKey);

    return true;
}

bool CContractInvokeTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    CKeyID keyId;
    if (!cw.pAccountCache->GetKeyId(txUid, keyId))
        return false;

    keyIds.insert(keyId);
    CKeyID desKeyId;
    if (!cw.pAccountCache->GetKeyId(appUid, desKeyId))
        return false;

    keyIds.insert(desKeyId);

    CVmRunEnv vmRunEnv;
    std::shared_ptr<CBaseTx> pTx = GetNewInstance();
    uint64_t fuelRate = GetFuelRate(*cw.pContractCache);
    CContractCache scriptDBView(*cw.pContractCache);

    if (!pCdMan->pTxCache->HaveTx(GetHash())) {
        tuple<bool, uint64_t, string> ret =
            vmRunEnv.ExecuteContract(pTx, chainActive.Height() + 1, cw, fuelRate, nRunStep);

        if (!std::get<0>(ret))
            return ERRORMSG("CContractInvokeTx::GetInvolvedKeyIds, %s", std::get<2>(ret));

        vector<shared_ptr<CAccount> > vpAccount = vmRunEnv.GetNewAccount();

        for (auto & item : vpAccount)
            keyIds.insert(item->keyID);

        vector<std::shared_ptr<CAppUserAccount> > &vAppUserAccount = vmRunEnv.GetRawAppUserAccount();
        for (auto & itemUserAccount : vAppUserAccount) {
            CKeyID itemKeyID;
            bool bValid = GetKeyId(*cw.pAccountCache, itemUserAccount.get()->GetAccUserId(), itemKeyID);
            if (bValid)
                keyIds.insert(itemKeyID);
        }
    } else {
        set<CKeyID> vTxRelAccount;
        if (!cw.pContractCache->GetTxRelAccount(GetHash(), vTxRelAccount))
            return false;

        keyIds.insert(vTxRelAccount.begin(), vTxRelAccount.end());
    }
    return true;
}

string CContractInvokeTx::ToString(CAccountCache &view) {
    string desId;
    if (txUid.type() == typeid(CKeyID)) {
        desId = appUid.get<CKeyID>().ToString();
    } else if (appUid.type() == typeid(CRegID)) {
        desId = appUid.get<CRegID>().ToString();
    }

    string str = strprintf(
        "txType=%s, hash=%s, ver=%d, srcId=%s, desId=%s, bcoins=%ld, llFees=%ld, arguments=%s, "
        "nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString().c_str(), nVersion,
        txUid.get<CRegID>().ToString(), desId.c_str(), bcoins, llFees,
        HexStr(arguments).c_str(), nValidHeight);

    return str;
}

Object CContractInvokeTx::ToJson(const CAccountCache &accountView) const {
    Object result;
    CAccountCache view(accountView);

    // auto GetRegIdString = [&](CUserID const &userId) {
    //     if (userId.type() == typeid(CRegID))
    //         return userId.get<CRegID>().ToString();
    //     return string("");
    // };

    CKeyID srcKeyId, desKeyId;
    view.GetKeyId(txUid, srcKeyId);
    view.GetKeyId(appUid, desKeyId);

    result.push_back(Pair("hash",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("regid",          txUid.ToString()));
    result.push_back(Pair("addr",           srcKeyId.ToAddress()));
    result.push_back(Pair("app_uid",        appUid.ToString()));
    result.push_back(Pair("app_addr",       desKeyId.ToAddress()));
    result.push_back(Pair("money",          bcoins));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("arguments",      HexStr(arguments)));
    result.push_back(Pair("valid_height",   nValidHeight));

    return result;
}

bool CContractInvokeTx::ExecuteTx(int nHeight, int nIndex, CCacheWrapper &cw, CValidationState &state) {
    CAccount srcAcct;
    CAccount desAcct;
    CAccountLog desAcctLog;
    uint64_t minusValue = llFees + bcoins;
    if (!cw.pAccountCache->GetAccount(txUid, srcAcct))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, read source addr %s account info error",
            txUid.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");

    CAccountLog srcAcctLog(srcAcct);
    if (!srcAcct.OperateBalance(CoinType::WICC, MINUS_VALUE, minusValue))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, accounts insufficient funds"),
            UPDATE_ACCOUNT_FAIL, "operate-minus-account-failed");

    CUserID userId = srcAcct.keyID;
    if (!cw.pAccountCache->SetAccount(userId, srcAcct))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, save account%s info error",
            txUid.get<CRegID>().ToString()), WRITE_ACCOUNT_FAIL, "bad-write-accountdb");

    if (!cw.pAccountCache->GetAccount(appUid, desAcct)) {
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, get account info failed by regid:%s",
            appUid.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");
    } else {
        desAcctLog.SetValue(desAcct);
    }

    if (!desAcct.OperateBalance(CoinType::WICC, ADD_VALUE, bcoins)) {
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, operate accounts error"),
                        UPDATE_ACCOUNT_FAIL, "operate-add-account-failed");
    }

    if (!cw.pAccountCache->SetAccount(appUid, desAcct))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, save account error, kyeId=%s",
            desAcct.keyID.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-account");

    cw.pTxUndo->vAccountLog.push_back(srcAcctLog);
    cw.pTxUndo->vAccountLog.push_back(desAcctLog);

    vector<unsigned char> vScript;
    if (!cw.pContractCache->GetScript(appUid.get<CRegID>(), vScript))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, read script failed, regId=%s",
            appUid.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-script");

    CVmRunEnv vmRunEnv;
    std::shared_ptr<CBaseTx> pTx = GetNewInstance();
    uint64_t fuelRate = GetFuelRate(*cw.pContractCache);

    int64_t llTime = GetTimeMillis();
    tuple<bool, uint64_t, string> ret = vmRunEnv.ExecuteContract(pTx, nHeight, cw, fuelRate, nRunStep);
    if (!std::get<0>(ret))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, txid=%s run script error:%s",
            GetHash().GetHex(), std::get<2>(ret)), UPDATE_ACCOUNT_FAIL, "run-script-error: " + std::get<2>(ret));

    LogPrint("vm", "execute contract elapse:%lld, txhash=%s\n", GetTimeMillis() - llTime, GetHash().GetHex());

    set<CKeyID> vAddress;
    vector<std::shared_ptr<CAccount> > &vAccount = vmRunEnv.GetNewAccount();
    // update accounts' info referred to the contract
    for (auto &itemAccount : vAccount) {
        vAddress.insert(itemAccount->keyID);
        userId = itemAccount->keyID;
        CAccount oldAcct;
        if (!cw.pAccountCache->GetAccount(userId, oldAcct)) {
            // The contract transfers money to an address for the first time.
            if (!itemAccount->keyID.IsNull()) {
                oldAcct.keyID = itemAccount->keyID;
            } else {
                return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, read account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
            }
        }
        CAccountLog oldAcctLog(oldAcct);
        if (!cw.pAccountCache->SetAccount(userId, *itemAccount))
            return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, write account info error"),
                UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");

        cw.pTxUndo->vAccountLog.push_back(oldAcctLog);
    }
    cw.pTxUndo->vContractOperLog.insert(cw.pTxUndo->vContractOperLog.end(), vmRunEnv.GetDbLog()->begin(),
                                 vmRunEnv.GetDbLog()->end());
    vector<std::shared_ptr<CAppUserAccount> > &vAppUserAccount = vmRunEnv.GetRawAppUserAccount();
    for (auto & itemUserAccount : vAppUserAccount) {
        CKeyID itemKeyID;
        bool bValid = GetKeyId(*cw.pAccountCache, itemUserAccount.get()->GetAccUserId(), itemKeyID);
        if (bValid)
            vAddress.insert(itemKeyID);
    }

    if (!cw.pContractCache->SetTxRelAccout(GetHash(), vAddress))
        return ERRORMSG("CContractInvokeTx::ExecuteTx, save tx relate account info to script db error");

    cw.pTxUndo->txHash = GetHash();

    if (SysCfg().GetAddressToTxFlag()) {
        CContractDBOperLog operAddressToTxLog;
        CKeyID sendKeyId;
        CKeyID revKeyId;
        if (!cw.pAccountCache->GetKeyId(txUid, sendKeyId))
            return ERRORMSG("CContractInvokeTx::ExecuteTx, get keyid by txUid error!");

        if (!cw.pAccountCache->GetKeyId(appUid, revKeyId))
            return ERRORMSG("CContractInvokeTx::ExecuteTx, get keyid by appUid error!");

        if (!cw.pContractCache->SetTxHashByAddress(sendKeyId, nHeight, nIndex + 1, cw.pTxUndo->txHash.GetHex(),
                                         operAddressToTxLog))
            return false;

        cw.pTxUndo->vContractOperLog.push_back(operAddressToTxLog);

        if (!cw.pContractCache->SetTxHashByAddress(revKeyId, nHeight, nIndex + 1, cw.pTxUndo->txHash.GetHex(),
                                         operAddressToTxLog))
            return false;

        cw.pTxUndo->vContractOperLog.push_back(operAddressToTxLog);
    }

    return true;
}

bool CContractInvokeTx::UndoExecuteTx(int nHeight, int nIndex, CCacheWrapper &cw, CValidationState &state) {
    vector<CAccountLog>::reverse_iterator rIterAccountLog = cw.pTxUndo->vAccountLog.rbegin();
    for (; rIterAccountLog != cw.pTxUndo->vAccountLog.rend(); ++rIterAccountLog) {
        CAccount account;
        CUserID userId = rIterAccountLog->keyID;
        if (!cw.pAccountCache->GetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CContractInvokeTx::UndoExecuteTx, read account info error"),
                             READ_ACCOUNT_FAIL, "bad-read-accountdb");
        }

        if (!account.UndoOperateAccount(*rIterAccountLog)) {
            return state.DoS(100,
                             ERRORMSG("CContractInvokeTx::UndoExecuteTx, undo operate account failed"),
                             UPDATE_ACCOUNT_FAIL, "undo-operate-account-failed");
        }

        if (!cw.pAccountCache->SetAccount(userId, account)) {
            return state.DoS(100, ERRORMSG("CContractInvokeTx::UndoExecuteTx, write account info error"),
                             UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
        }
    }

    vector<CContractDBOperLog>::reverse_iterator rIterScriptDBLog = cw.pTxUndo->vContractOperLog.rbegin();
    for (; rIterScriptDBLog != cw.pTxUndo->vContractOperLog.rend(); ++rIterScriptDBLog) {
        if (!cw.pContractCache->UndoScriptData(rIterScriptDBLog->vKey, rIterScriptDBLog->vValue))
            return state.DoS(100, ERRORMSG("CContractInvokeTx::UndoExecuteTx, undo scriptdb data error"),
                             UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    if (!cw.pContractCache->EraseTxRelAccout(GetHash()))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::UndoExecuteTx, erase tx rel account error"),
                         UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");

    return true;
}

bool CContractInvokeTx::CheckTx(CCacheWrapper &cw, CValidationState &state) {
    IMPLEMENT_CHECK_TX_FEE;
    IMPLEMENT_CHECK_TX_ARGUMENTS;
    IMPLEMENT_CHECK_TX_REGID(txUid.type());
    IMPLEMENT_CHECK_TX_REGID(appUid.type());

    CAccount srcAccount;
    if (!cw.pAccountCache->GetAccount(txUid, srcAccount))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, read account failed, regId=%s",
                        txUid.get<CRegID>().ToString()), REJECT_INVALID, "bad-getaccount");

    if (!srcAccount.IsRegistered())
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, account pubkey not registered"),
                        REJECT_INVALID, "bad-account-unregistered");

    vector<unsigned char> vScript;
    if (!cw.pContractCache->GetScript(appUid.get<CRegID>(), vScript))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, read script failed, regId=%s",
                        appUid.get<CRegID>().ToString()), REJECT_INVALID, "bad-read-script");

    IMPLEMENT_CHECK_TX_SIGNATURE(srcAccount.pubKey);

    return true;
}