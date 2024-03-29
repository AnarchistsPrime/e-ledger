#include "main.h"
#include "zerocoin.h"
#include "timedata.h"
#include "chainparams.h"
#include "util.h"
#include "base58.h"
#include "definition.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"

#include <atomic>
#include <sstream>
#include <chrono>

#include <boost/foreach.hpp>

using namespace std;

// Settings
int64_t nTransactionFee = 0;
int64_t nMinimumInputValue = DUST_HARD_LIMIT;

// btzc: add zerocoin init
// zerocoin init
static CBigNum bnTrustedModulus;
bool setParams = bnTrustedModulus.SetHexBool(ZEROCOIN_MODULUS);

// Set up the Zerocoin Params object
uint32_t securityLevel = 80;
static libzerocoin::Params *ZCParams = new libzerocoin::Params(bnTrustedModulus);

static CZerocoinState zerocoinState;

bool CheckSpendEledgerTransaction(const CTransaction &tx,
                                libzerocoin::CoinDenomination targetDenomination,
                                CValidationState &state,
                                uint256 hashTx,
                                bool isVerifyDB,
                                int nHeight,
                                bool isCheckWallet,
                                CZerocoinTxInfo *zerocoinTxInfo) {

    // Check for inputs only, everything else was checked before
	LogPrintf("CheckSpendEledgerTransaction denomination=%d nHeight=%d\n", targetDenomination, nHeight);

	BOOST_FOREACH(const CTxIn &txin, tx.vin)
	{
        if (!txin.scriptSig.IsZerocoinSpend())
            continue;

        uint32_t pubcoinId = txin.nSequence;
        if (pubcoinId < 1 || pubcoinId >= INT_MAX) {
             // coin id should be positive integer
            return state.DoS(100,
                false,
                NSEQUENCE_INCORRECT,
                "CTransaction::CheckTransaction() : Error: zerocoin spend nSequence is incorrect");
        }

        if (txin.scriptSig.size() < 4)
            return state.DoS(100,
                             false,
                             REJECT_MALFORMED,
                             "CheckSpendEledgerTransaction: invalid spend transaction");

        // Deserialize the CoinSpend intro a fresh object
        CDataStream serializedCoinSpend((const char *)&*(txin.scriptSig.begin() + 4),
                                        (const char *)&*txin.scriptSig.end(),
                                        SER_NETWORK, PROTOCOL_VERSION);
        libzerocoin::CoinSpend newSpend(ZCParams, serializedCoinSpend);

        int spendVersion = newSpend.getVersion();
        if (spendVersion != ZEROCOIN_TX_VERSION_1 &&
                spendVersion != ZEROCOIN_TX_VERSION_1_5 &&
                spendVersion != ZEROCOIN_TX_VERSION_2) {
            return state.DoS(100,
                false,
                NSEQUENCE_INCORRECT,
                "CTransaction::CheckTransaction() : Error: incorrect spend transaction verion");
        }

        if (IsZerocoinTxV2(targetDenomination, pubcoinId)) {
            // After threshold id all spends should be either version 1.5 or 2.0
            if (spendVersion == ZEROCOIN_TX_VERSION_1)
                return state.DoS(100,
                    false,
                    NSEQUENCE_INCORRECT,
                    "CTransaction::CheckTransaction() : Error: zerocoin spend should be version 1.5 or 2.0");
        }
        else {
            // old spends v2.0s are probably incorrect, force spend to version 1
            if (spendVersion == ZEROCOIN_TX_VERSION_2) {
                spendVersion = ZEROCOIN_TX_VERSION_1;
                newSpend.setVersion(ZEROCOIN_TX_VERSION_1);
            }
        }

        uint256 txHashForMetadata;

        if (spendVersion > ZEROCOIN_TX_VERSION_1) {
            // Obtain the hash of the transaction sans the zerocoin part
            CMutableTransaction txTemp = tx;
            BOOST_FOREACH(CTxIn &txTempIn, txTemp.vin) {
                if (txTempIn.scriptSig.IsZerocoinSpend()) {
                    txTempIn.scriptSig.clear();
                    txTempIn.prevout.SetNull();
                }
            }
            txHashForMetadata = txTemp.GetHash();
        }

        LogPrintf("CheckSpendEledgerTransaction: tx version=%d, tx metadata hash=%s, serial=%s\n", newSpend.getVersion(), txHashForMetadata.ToString(), newSpend.getCoinSerialNumber().ToString());

        if (spendVersion == ZEROCOIN_TX_VERSION_1 && nHeight == INT_MAX) {
            bool fTestNet = Params().NetworkIDString() == CBaseChainParams::TESTNET;
            int txHeight;
            txHeight = chainActive.Height();
            int allowedV1Height = fTestNet ? ZC_V1_5_TESTNET_STARTING_BLOCK : ZC_V1_5_STARTING_BLOCK;
            if (txHeight >= allowedV1Height + ZC_V1_5_GRACEFUL_MEMPOOL_PERIOD) {
                LogPrintf("CheckSpendEledgerTransaction: cannot allow spend v1 into mempool after block %d\n",
                          allowedV1Height + ZC_V1_5_GRACEFUL_MEMPOOL_PERIOD);
                return false;
            }
        }

        libzerocoin::SpendMetaData newMetadata(txin.nSequence, txHashForMetadata);

        CZerocoinState::CoinGroupInfo coinGroup;
        if (!zerocoinState.GetCoinGroupInfo(targetDenomination, pubcoinId, coinGroup))
            return state.DoS(100, false, NO_MINT_ZEROCOIN, "CheckSpendEledgerTransaction: Error: no coins were minted with such parameters");

        bool passVerify = false;
        CBlockIndex *index = coinGroup.lastBlock;
        pair<int,int> denominationAndId = make_pair(targetDenomination, pubcoinId);
		
		bool spendHasBlockHash = false;
		
        // Zerocoin v1.5/v2 transaction can cointain block hash of the last mint tx seen at the moment of spend. It speeds
		// up verification
        if (spendVersion > ZEROCOIN_TX_VERSION_1 && !newSpend.getAccumulatorBlockHash().IsNull()) {
			spendHasBlockHash = true;
			uint256 accumulatorBlockHash = newSpend.getAccumulatorBlockHash();
			
			// find index for block with hash of accumulatorBlockHash or set index to the coinGroup.firstBlock if not found
			while (index != coinGroup.firstBlock && index->GetBlockHash() != accumulatorBlockHash)
				index = index->pprev;
		}

        // Enumerate all the accumulator changes seen in the blockchain starting with the latest block
        // In most cases the latest accumulator value will be used for verification
        do {
            if (index->accumulatorChanges.count(denominationAndId) > 0) {						
                libzerocoin::Accumulator accumulator(ZCParams,
                                                     index->accumulatorChanges[denominationAndId].first,
                                                     targetDenomination);
                LogPrintf("CheckSpendEledgerTransaction: accumulator=%s\n", accumulator.getValue().ToString().substr(0,15));
                passVerify = newSpend.Verify(accumulator, newMetadata);
            }

	        // if spend has block hash we don't need to look further
			if (index == coinGroup.firstBlock || spendHasBlockHash)
                break;
            else
                index = index->pprev;
        } while (!passVerify);

        // Rare case: accumulator value contains some but NOT ALL coins from one block. In this case we will
        // have to enumerate over coins manually. No optimization is really needed here because it's a rarity
        // This can't happen if spend is of version 1.5 or 2.0
        if (!passVerify && spendVersion == ZEROCOIN_TX_VERSION_1) {
            // Build vector of coins sorted by the time of mint
            index = coinGroup.lastBlock;
            vector<CBigNum> pubCoins = index->mintedPubCoins[denominationAndId];
            if (index != coinGroup.firstBlock) {
                do {
                    index = index->pprev;
                    if (index->mintedPubCoins.count(denominationAndId) > 0)
                        pubCoins.insert(pubCoins.begin(),
                                        index->mintedPubCoins[denominationAndId].cbegin(),
                                        index->mintedPubCoins[denominationAndId].cend());
                } while (index != coinGroup.firstBlock);
            }

            libzerocoin::Accumulator accumulator(ZCParams, targetDenomination);
            BOOST_FOREACH(const CBigNum &pubCoin, pubCoins) {
                accumulator += libzerocoin::PublicCoin(ZCParams, pubCoin, (libzerocoin::CoinDenomination)targetDenomination);
                LogPrintf("CheckSpendEledgerTransaction: accumulator=%s\n", accumulator.getValue().ToString().substr(0,15));
                if ((passVerify = newSpend.Verify(accumulator, newMetadata)) == true)
                    break;
            }

            if (!passVerify) {
                // One more time now in reverse direction. The only reason why it's required is compatibility with
                // previous client versions
                libzerocoin::Accumulator accumulator(ZCParams, targetDenomination);
                BOOST_REVERSE_FOREACH(const CBigNum &pubCoin, pubCoins) {
                    accumulator += libzerocoin::PublicCoin(ZCParams, pubCoin, (libzerocoin::CoinDenomination)targetDenomination);
                    LogPrintf("CheckSpendEledgerTransaction: accumulatorRev=%s\n", accumulator.getValue().ToString().substr(0,15));
                    if ((passVerify = newSpend.Verify(accumulator, newMetadata)) == true)
                        break;
                }
            }
        }


        if (passVerify) {
            // Pull the serial number out of the CoinSpend object. If we
            // were a real Zerocoin client we would now check that the serial number
            // has not been spent before (in another ZEROCOIN_SPEND) transaction.
            // The serial number is stored as a Bignum.
            CBigNum serial = newSpend.getCoinSerialNumber();
            if (nHeight > ZC_CHECK_BUG_FIXED_AT_BLOCK &&
                    // do not check for duplicates in case we've seen exact copy of this tx in this block before
                    !(zerocoinTxInfo &&
                        zerocoinTxInfo->zcTransactions.count(hashTx) > 0) &&
                    // check for used serials both in zerocoinState and in other transactions of this block
                    (zerocoinState.IsUsedCoinSerial(serial) ||
                        // check for zerocoin transaction in the same block as well
                        (zerocoinTxInfo &&
                            !zerocoinTxInfo->fInfoIsComplete &&
                         zerocoinTxInfo->spentSerials.count(serial) > 0))) {

                if (nHeight < ZC_V1_5_STARTING_BLOCK)
                    LogPrintf("ZCSpend: height=%d, denomination=%d, serial=%s\n", nHeight, (int)newSpend.getDenomination(), newSpend.getCoinSerialNumber().ToString());
                else
                    return state.DoS(0, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
            }

            if(!isVerifyDB && !isCheckWallet) {
                if (zerocoinTxInfo && !zerocoinTxInfo->fInfoIsComplete) {
                    // add spend information to the index
                    zerocoinTxInfo->spentSerials.insert(serial);
                    zerocoinTxInfo->zcTransactions.insert(hashTx);

                    if (newSpend.getVersion() == ZEROCOIN_TX_VERSION_1)
                        zerocoinTxInfo->fHasSpendV1 = true;
                }
            }
        }
        else {
            LogPrintf("CheckSpendeledgerTransaction: verification failed at block %d\n", nHeight);
            return false;
        }
	}
	return true;
}

bool CheckMintEledgerTransaction(const CTxOut &txout,
                               CValidationState &state,
                               uint256 hashTx,
                               CZerocoinTxInfo *zerocoinTxInfo) {

    LogPrintf("CheckMintEledgerTransaction txHash = %s\n", txout.GetHash().ToString());
    LogPrintf("nValue = %d\n", txout.nValue);

    if (txout.scriptPubKey.size() < 6)
        return state.DoS(100,
            false,
            PUBCOIN_NOT_VALIDATE,
            "CTransaction::CheckTransaction() : PubCoin validation failed");

    CBigNum pubCoin(vector<unsigned char>(txout.scriptPubKey.begin()+6, txout.scriptPubKey.end()));

    bool hasCoin = zerocoinState.HasCoin(pubCoin);

    if (!hasCoin && zerocoinTxInfo && !zerocoinTxInfo->fInfoIsComplete) {
        BOOST_FOREACH(const PAIRTYPE(int,CBigNum) &mint, zerocoinTxInfo->mints) {
            if (mint.second == pubCoin) {
                hasCoin = true;
                break;
            }
        }
    }

    if (hasCoin) {
        /*return state.DoS(100,
                         false,
                         PUBCOIN_NOT_VALIDATE,
                         "CheckZerocoinTransaction: duplicate mint");*/
        LogPrintf("CheckMintZerocoinTransaction: double mint, tx=%s\n", txout.GetHash().ToString());
    }

    switch (txout.nValue) {
    default:
        return state.DoS(100,
            false,
            PUBCOIN_NOT_VALIDATE,
            "CheckZerocoinTransaction : PubCoin denomination is invalid");

    case libzerocoin::ZQ_LOVELACE*COIN:
    case libzerocoin::ZQ_GOLDWASSER*COIN:
    case libzerocoin::ZQ_RACKOFF*COIN:
    case libzerocoin::ZQ_PEDERSEN*COIN:
    case libzerocoin::ZQ_WILLIAMSON*COIN:
        libzerocoin::CoinDenomination denomination = (libzerocoin::CoinDenomination)(txout.nValue / COIN);
        libzerocoin::PublicCoin checkPubCoin(ZCParams, pubCoin, denomination);
        if (!checkPubCoin.validate())
            return state.DoS(100,
                false,
                PUBCOIN_NOT_VALIDATE,
                "CheckZerocoinTransaction : PubCoin validation failed");

        if (zerocoinTxInfo != NULL && !zerocoinTxInfo->fInfoIsComplete) {
            // Update public coin list in the info
            zerocoinTxInfo->mints.push_back(make_pair(denomination, pubCoin));
            zerocoinTxInfo->zcTransactions.insert(hashTx);
        }

        break;
    }

    return true;
}

bool CheckZerocoinFoundersInputs(const CTransaction &tx, CValidationState &state, int nHeight, bool fTestNet) {
    // Check for founders inputs
    if (((nHeight > ZC_CHECK_BUG_FIXED_AT_BLOCK) && (nHeight < 210000)) || (fTestNet && nHeight >= 7200)) {
        bool found_1 = false;
        bool found_2 = false;
        bool found_3 = false;
        bool found_4 = false;
        bool found_5 = false;
        int total_payment_tx = 0; // no more than 1 output for payment
        CScript FOUNDER_1_SCRIPT;
        CScript FOUNDER_2_SCRIPT;
        CScript FOUNDER_3_SCRIPT;
        CScript FOUNDER_4_SCRIPT;
        CScript FOUNDER_5_SCRIPT;
        if (nHeight < Params().GetConsensus().nEnodePaymentsStartBlock) {
            if (!fTestNet && GetAdjustedTime() > nStartRewardTime) {
                FOUNDER_1_SCRIPT = GetScriptForDestination(CBitcoinAddress("aHb2hv9kVFb2gZM1BufzUo4pANkWHphkWy").Get());
                if (nHeight < 14000) {
                    FOUNDER_2_SCRIPT = GetScriptForDestination(
                            CBitcoinAddress("a8wUrPhGYUAoYtFR2TNn9usErhpwJcHjPE").Get());
                } else {
                    FOUNDER_2_SCRIPT = GetScriptForDestination(
                            CBitcoinAddress("a8wUrPhGYUAoYtFR2TNn9usErhpwJcHjPE").Get());
                }
                FOUNDER_3_SCRIPT = GetScriptForDestination(CBitcoinAddress("a8QVfSnptYUJNBavKeST6qJu3HqfghY7q8").Get());
                FOUNDER_4_SCRIPT = GetScriptForDestination(CBitcoinAddress("a7DVbtA54KPGKknhM2rmFCZvkL5accBspn").Get());
                FOUNDER_5_SCRIPT = GetScriptForDestination(CBitcoinAddress("aNrU6t6m9wmrkxmtUuq3rFtdTDgL55QSdA").Get());
            } else if (!fTestNet && GetAdjustedTime() <= nStartRewardTime) {
                return state.DoS(100, false, REJECT_TRANSACTION_TOO_EARLY,
                                 "CTransaction::CheckTransaction() : transaction is too early");
            } else {
                FOUNDER_1_SCRIPT = GetScriptForDestination(CBitcoinAddress("TDk19wPKYq91i18qmY6U9FeTdTxwPeSveo").Get());
                FOUNDER_2_SCRIPT = GetScriptForDestination(CBitcoinAddress("TWZZcDGkNixTAMtRBqzZkkMHbq1G6vUTk5").Get());
                FOUNDER_3_SCRIPT = GetScriptForDestination(CBitcoinAddress("TRZTFdNCKCKbLMQV8cZDkQN9Vwuuq4gDzT").Get());
                FOUNDER_4_SCRIPT = GetScriptForDestination(CBitcoinAddress("TG2ruj59E5b1u9G3F7HQVs6pCcVDBxrQve").Get());
                FOUNDER_5_SCRIPT = GetScriptForDestination(CBitcoinAddress("TCsTzQZKVn4fao8jDmB9zQBk9YQNEZ3XfS").Get());
            }

            BOOST_FOREACH(const CTxOut &output, tx.vout) {
                if (output.scriptPubKey == FOUNDER_1_SCRIPT && output.nValue == (int64_t)(2 * COIN)) {
                    found_1 = true;
                }
                if (output.scriptPubKey == FOUNDER_2_SCRIPT && output.nValue == (int64_t)(2 * COIN)) {
                    found_2 = true;
                }
                if (output.scriptPubKey == FOUNDER_3_SCRIPT && output.nValue == (int64_t)(2 * COIN)) {
                    found_3 = true;
                }
                if (output.scriptPubKey == FOUNDER_4_SCRIPT && output.nValue == (int64_t)(2 * COIN)) {
                    found_4 = true;
                }
                if (output.scriptPubKey == FOUNDER_5_SCRIPT && output.nValue == (int64_t)(2 * COIN)) {
                    found_5 = true;
                }
            }
        } else {

            if (!fTestNet && GetAdjustedTime() > nStartRewardTime) {
                FOUNDER_1_SCRIPT = GetScriptForDestination(CBitcoinAddress("aHb2hv9kVFb2gZM1BufzUo4pANkWHphkWy").Get());
                if (nHeight < 14000) {
                    FOUNDER_2_SCRIPT = GetScriptForDestination(
                            CBitcoinAddress("a8wUrPhGYUAoYtFR2TNn9usErhpwJcHjPE").Get());
                } else {
                    FOUNDER_2_SCRIPT = GetScriptForDestination(
                            CBitcoinAddress("a8wUrPhGYUAoYtFR2TNn9usErhpwJcHjPE").Get());
                }
                FOUNDER_3_SCRIPT = GetScriptForDestination(CBitcoinAddress("a8QVfSnptYUJNBavKeST6qJu3HqfghY7q8").Get());
                FOUNDER_4_SCRIPT = GetScriptForDestination(CBitcoinAddress("a7DVbtA54KPGKknhM2rmFCZvkL5accBspn").Get());
                FOUNDER_5_SCRIPT = GetScriptForDestination(CBitcoinAddress("aNrU6t6m9wmrkxmtUuq3rFtdTDgL55QSdA").Get());
            } else if (!fTestNet && GetAdjustedTime() <= nStartRewardTime) {
                return state.DoS(100, false, REJECT_TRANSACTION_TOO_EARLY,
                                 "CTransaction::CheckTransaction() : transaction is too early");
            } else {
                FOUNDER_1_SCRIPT = GetScriptForDestination(CBitcoinAddress("TDk19wPKYq91i18qmY6U9FeTdTxwPeSveo").Get());
                FOUNDER_2_SCRIPT = GetScriptForDestination(CBitcoinAddress("TWZZcDGkNixTAMtRBqzZkkMHbq1G6vUTk5").Get());
                FOUNDER_3_SCRIPT = GetScriptForDestination(CBitcoinAddress("TRZTFdNCKCKbLMQV8cZDkQN9Vwuuq4gDzT").Get());
                FOUNDER_4_SCRIPT = GetScriptForDestination(CBitcoinAddress("TG2ruj59E5b1u9G3F7HQVs6pCcVDBxrQve").Get());
                FOUNDER_5_SCRIPT = GetScriptForDestination(CBitcoinAddress("TCsTzQZKVn4fao8jDmB9zQBk9YQNEZ3XfS").Get());
            }

            CAmount znodePayment = GetEnodePayment(nHeight);
            BOOST_FOREACH(const CTxOut &output, tx.vout) {
                if (output.scriptPubKey == FOUNDER_1_SCRIPT && output.nValue == (int64_t)(1 * COIN)) {
                    found_1 = true;
                    continue;
                }
                if (output.scriptPubKey == FOUNDER_2_SCRIPT && output.nValue == (int64_t)(1 * COIN)) {
                    found_2 = true;
                    continue;
                }
                if (output.scriptPubKey == FOUNDER_3_SCRIPT && output.nValue == (int64_t)(1 * COIN)) {
                    found_3 = true;
                    continue;
                }
                if (output.scriptPubKey == FOUNDER_4_SCRIPT && output.nValue == (int64_t)(3 * COIN)) {
                    found_4 = true;
                    continue;
                }
                if (output.scriptPubKey == FOUNDER_5_SCRIPT && output.nValue == (int64_t)(1 * COIN)) {
                    found_5 = true;
                    continue;
                }
                if (znodePayment == output.nValue) {
                    total_payment_tx = total_payment_tx + 1;
                }
            }
        }

        if (!(found_1 && found_2 && found_3 && found_4 && found_5)) {
            return state.DoS(100, false, REJECT_FOUNDER_REWARD_MISSING,
                             "CTransaction::CheckTransaction() : founders reward missing");
        }

        if (total_payment_tx > 1) {
            return state.DoS(100, false, REJECT_INVALID_ENODE_PAYMENT,
                             "CTransaction::CheckTransaction() : invalid znode payment");
        }
    }

	return true;
}

bool CheckZerocoinTransaction(const CTransaction &tx,
                              CValidationState &state,
                              uint256 hashTx,
                              bool isVerifyDB,
                              int nHeight,
                              bool isCheckWallet,
                              CZerocoinTxInfo *zerocoinTxInfo)
{
	// Check Mint Zerocoin Transaction
	BOOST_FOREACH(const CTxOut &txout, tx.vout) {
		if (!txout.scriptPubKey.empty() && txout.scriptPubKey.IsZerocoinMint()) {
            if (!CheckMintEledgerTransaction(txout, state, hashTx, zerocoinTxInfo))
                return false;
		}
	}

	// Check Spend Zerocoin Transaction
	if(tx.IsZerocoinSpend()) {
		// Check vOut
		// Only one loop, we checked on the format before enter this case
		BOOST_FOREACH(const CTxOut &txout, tx.vout)
		{
			if (!isVerifyDB) {
                switch (txout.nValue) {
                case libzerocoin::ZQ_LOVELACE*COIN:
                case libzerocoin::ZQ_GOLDWASSER*COIN:
                case libzerocoin::ZQ_RACKOFF*COIN:
                case libzerocoin::ZQ_PEDERSEN*COIN:
                case libzerocoin::ZQ_WILLIAMSON*COIN:
                    if(!CheckSpendEledgerTransaction(tx, (libzerocoin::CoinDenomination)(txout.nValue / COIN), state, hashTx, isVerifyDB, nHeight, isCheckWallet, zerocoinTxInfo))
                        return false;
                    break;

                default:
                    return state.DoS(100, error("CheckZerocoinTransaction : invalid spending txout value"));
                }
			}
		}
	}
	
	return true;
}

void DisconnectTipZC(CBlock & /*block*/, CBlockIndex *pindexDelete) {
    zerocoinState.RemoveBlock(pindexDelete);

    // TODO: notify the wallet
}


/**
 * Connect a new ZCblock to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 */
bool ConnectTipZC(CValidationState &state, const CChainParams &chainparams, CBlockIndex *pindexNew, const CBlock *pblock) {

    // Add zerocoin transaction information to index
    if (pblock && pblock->zerocoinTxInfo) {
        if (pblock->zerocoinTxInfo->fHasSpendV1) {
            // Don't allow spend v1s after some point of time
            int allowV1Height =
                    chainparams.NetworkIDString() == CBaseChainParams::TESTNET ?
                        ZC_V1_5_TESTNET_STARTING_BLOCK : ZC_V1_5_STARTING_BLOCK;
            if (pindexNew->nHeight >= allowV1Height + ZC_V1_5_GRACEFUL_PERIOD) {
                LogPrintf("ConnectTipZC: spend v1 is not allowed after block %d\n", allowV1Height);
                return false;
            }
        }

        pindexNew->spentSerials = pblock->zerocoinTxInfo->spentSerials;
        if (pindexNew->nHeight > ZC_CHECK_BUG_FIXED_AT_BLOCK) {
            BOOST_FOREACH(const CBigNum &serial, pindexNew->spentSerials) {
                zerocoinState.AddSpend(serial);
            }
        }

        // Update minted values and accumulators
        BOOST_FOREACH(const PAIRTYPE(int,CBigNum) &mint, pblock->zerocoinTxInfo->mints) {
            int denomination = mint.first;
            CBigNum oldAccValue = ZCParams->accumulatorParams.accumulatorBase;
            int mintId = zerocoinState.AddMint(pindexNew, denomination, mint.second, oldAccValue);
            LogPrintf("ConnectTipZC: mint added denomination=%d, id=%d\n", denomination, mintId);
            pair<int,int> denomAndId = make_pair(denomination, mintId);

            pindexNew->mintedPubCoins[denomAndId].push_back(mint.second);

            CZerocoinState::CoinGroupInfo coinGroupInfo;
            zerocoinState.GetCoinGroupInfo(denomination, mintId, coinGroupInfo);

            libzerocoin::PublicCoin pubCoin(ZCParams, mint.second, (libzerocoin::CoinDenomination)denomination);
            libzerocoin::Accumulator accumulator(ZCParams,
                                                 oldAccValue,
                                                 (libzerocoin::CoinDenomination)denomination);
            accumulator += pubCoin;

            if (pindexNew->accumulatorChanges.count(denomAndId) > 0) {
                pair<CBigNum,int> &accChange = pindexNew->accumulatorChanges[denomAndId];
                accChange.first = accumulator.getValue();
                accChange.second++;
            }
            else {
                pindexNew->accumulatorChanges[denomAndId] = make_pair(accumulator.getValue(), 1);
            }
        }               
    }
    else {
        zerocoinState.AddBlock(pindexNew);
    }

	return true;
}

int ZerocoinGetNHeight(const CBlockHeader &block) {
	CBlockIndex *pindexPrev = NULL;
	int nHeight = 0;
	BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
	if (mi != mapBlockIndex.end()) {
		pindexPrev = (*mi).second;
		nHeight = pindexPrev->nHeight + 1;
	}
	return nHeight;
}


bool ZerocoinBuildStateFromIndex(CChain *chain) {
    zerocoinState.Reset();
    for (CBlockIndex *blockIndex = chain->Genesis(); blockIndex; blockIndex=chain->Next(blockIndex))
        zerocoinState.AddBlock(blockIndex);

    // DEBUG
    LogPrintf("Latest IDs are %d, %d, %d, %d, %d\n",
              zerocoinState.latestCoinIds[1],
               zerocoinState.latestCoinIds[10],
            zerocoinState.latestCoinIds[25],
            zerocoinState.latestCoinIds[50],
            zerocoinState.latestCoinIds[100]);
	return true;
}

// CZerocoinTxInfo

void CZerocoinTxInfo::Complete() {
    // We need to sort mints lexicographically by serialized value of pubCoin. That's the way old code
    // works, we need to stick to it. Denomination doesn't matter but we will sort by it as well
    sort(mints.begin(), mints.end(),
         [](decltype(mints)::const_reference m1, decltype(mints)::const_reference m2)->bool {
            CDataStream ds1(SER_DISK, CLIENT_VERSION), ds2(SER_DISK, CLIENT_VERSION);
            ds1 << m1.second;
            ds2 << m2.second;
            return (m1.first < m2.first) || ((m1.first == m2.first) && (ds1.str() < ds2.str()));
         });

    // Mark this info as complete
    fInfoIsComplete = true;
}

// CZerocoinState::CBigNumHash

std::size_t CZerocoinState::CBigNumHash::operator ()(const CBigNum &bn) const noexcept {
    // we are operating on almost random big numbers and least significant bytes (save for few last bytes) give us a good hash
    vector<unsigned char> bnData = bn.ToBytes();
    if (bnData.size() < sizeof(size_t)*3)
        // rare case, put ones like that into one hash bin
        return 0;
    else
        return ((size_t*)bnData.data())[1];
}

// CZerocoinState

CZerocoinState::CZerocoinState() {
}

int CZerocoinState::AddMint(CBlockIndex *index, int denomination, const CBigNum &pubCoin, CBigNum &previousAccValue) {

    int     mintId = 1;

    if (latestCoinIds[denomination] < 1)
        latestCoinIds[denomination] = mintId;
    else
        mintId = latestCoinIds[denomination];

    // There is a limit of 10 coins per group but mints belonging to the same block must have the same id thus going
    // beyond 10
    CoinGroupInfo &coinGroup = coinGroups[make_pair(denomination, mintId)];
	int coinsPerId = IsZerocoinTxV2((libzerocoin::CoinDenomination)denomination, mintId) ? ZC_SPEND_V2_COINSPERID : ZC_SPEND_V1_COINSPERID;
    if (coinGroup.nCoins < coinsPerId || coinGroup.lastBlock == index) {
        if (coinGroup.nCoins++ == 0) {
            // first groups of coins for given denomination
            coinGroup.firstBlock = coinGroup.lastBlock = index;
        }
        else {
            previousAccValue = coinGroup.lastBlock->accumulatorChanges[make_pair(denomination,mintId)].first;
            coinGroup.lastBlock = index;
        }
    }
    else {
        latestCoinIds[denomination] = ++mintId;
        CoinGroupInfo &newCoinGroup = coinGroups[make_pair(denomination, mintId)];
        newCoinGroup.firstBlock = newCoinGroup.lastBlock = index;
        newCoinGroup.nCoins = 1;
    }

    CMintedCoinInfo coinInfo;
    coinInfo.denomination = denomination;
    coinInfo.id = mintId;
    coinInfo.nHeight = index->nHeight;
    mintedPubCoins.insert(pair<CBigNum,CMintedCoinInfo>(pubCoin, coinInfo));

    return mintId;
}

void CZerocoinState::AddSpend(const CBigNum &serial) {
    usedCoinSerials.insert(serial);
}

void CZerocoinState::AddBlock(CBlockIndex *index) {
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int), PAIRTYPE(CBigNum,int)) &accUpdate, index->accumulatorChanges)
    {
        CoinGroupInfo   &coinGroup = coinGroups[accUpdate.first];

        if (coinGroup.firstBlock == NULL)
            coinGroup.firstBlock = index;
        coinGroup.lastBlock = index;
        coinGroup.nCoins += accUpdate.second.second;
    }

    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int),vector<CBigNum>) &pubCoins, index->mintedPubCoins) {
        latestCoinIds[pubCoins.first.first] = pubCoins.first.second;
        BOOST_FOREACH(const CBigNum &coin, pubCoins.second) {
            CMintedCoinInfo coinInfo;
            coinInfo.denomination = pubCoins.first.first;
            coinInfo.id = pubCoins.first.second;
            coinInfo.nHeight = index->nHeight;
            mintedPubCoins.insert(pair<CBigNum,CMintedCoinInfo>(coin, coinInfo));
        }
    }

    if (index->nHeight > ZC_CHECK_BUG_FIXED_AT_BLOCK) {
        BOOST_FOREACH(const CBigNum &serial, index->spentSerials) {
            usedCoinSerials.insert(serial);
        }
    }
}

void CZerocoinState::RemoveBlock(CBlockIndex *index) {
    // roll back accumulator updates
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int), PAIRTYPE(CBigNum,int)) &accUpdate, index->accumulatorChanges)
    {
        CoinGroupInfo   &coinGroup = coinGroups[accUpdate.first];
        int  nMintsToForget = accUpdate.second.second;

        assert(coinGroup.nCoins >= nMintsToForget);

        if ((coinGroup.nCoins -= nMintsToForget) == 0) {
            // all the coins of this group have been erased, remove the group altogether
            coinGroups.erase(accUpdate.first);
            // decrease pubcoin id for this denomination
            latestCoinIds[accUpdate.first.first]--;
        }
        else {
            // roll back lastBlock to previous position
            do {
                assert(coinGroup.lastBlock != coinGroup.firstBlock);
                coinGroup.lastBlock = coinGroup.lastBlock->pprev;
            } while (coinGroup.lastBlock->accumulatorChanges.count(accUpdate.first) == 0);
        }
    }

    // roll back mints
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int),vector<CBigNum>) &pubCoins, index->mintedPubCoins) {
        BOOST_FOREACH(const CBigNum &coin, pubCoins.second) {
            auto coins = mintedPubCoins.equal_range(coin);
            auto coinIt = find_if(coins.first, coins.second, [=](const decltype(mintedPubCoins)::value_type &v) {
                return v.second.denomination == pubCoins.first.first &&
                        v.second.id == pubCoins.first.second;
            });
            assert(coinIt != mintedPubCoins.end());
            mintedPubCoins.erase(coinIt);
        }
    }

    // roll back spends
    BOOST_FOREACH(const CBigNum &serial, index->spentSerials) {
        usedCoinSerials.erase(serial);
    }
}

bool CZerocoinState::GetCoinGroupInfo(int denomination, int id, CoinGroupInfo &result) {
    pair<int,int>   key = make_pair(denomination, id);
    if (coinGroups.count(key) == 0)
        return false;

    result = coinGroups[key];
    return true;
}

bool CZerocoinState::IsUsedCoinSerial(const CBigNum &coinSerial) {
    return usedCoinSerials.count(coinSerial) != 0;
}

bool CZerocoinState::HasCoin(const CBigNum &pubCoin) {
    return mintedPubCoins.count(pubCoin) != 0;
}

int CZerocoinState::GetAccumulatorValueForSpend(int maxHeight, int denomination, int id, CBigNum &accumulator, uint256 &blockHash) {
    pair<int, int> denomAndId = pair<int, int>(denomination, id);

    if (coinGroups.count(denomAndId) == 0)
        return 0;

    CoinGroupInfo coinGroup = coinGroups[denomAndId];
    CBlockIndex *lastBlock = coinGroup.lastBlock;

    assert(lastBlock->accumulatorChanges.count(denomAndId) > 0);
    assert(coinGroup.firstBlock->accumulatorChanges.count(denomAndId) > 0);

    int numberOfCoins = 0;
    for (;;) {
        if (lastBlock->accumulatorChanges.count(denomAndId) > 0) {
            if (lastBlock->nHeight <= maxHeight) {
                if (numberOfCoins == 0) {
                    // latest block satisfying given conditions
                    // remember accumulator value and block hash
                    accumulator = lastBlock->accumulatorChanges[denomAndId].first;
                    blockHash = lastBlock->GetBlockHash();
                }
                numberOfCoins += lastBlock->accumulatorChanges[denomAndId].second;
            }
        }

        if (lastBlock == coinGroup.firstBlock)
            break;
        else
            lastBlock = lastBlock->pprev;
    }

    return numberOfCoins;
}

libzerocoin::AccumulatorWitness CZerocoinState::GetWitnessForSpend(CChain *chain, int maxHeight, int denomination, int id, const CBigNum &pubCoin) {
    libzerocoin::CoinDenomination d = (libzerocoin::CoinDenomination)denomination;
    pair<int, int> denomAndId = pair<int, int>(denomination, id);

    assert(coinGroups.count(denomAndId) > 0);

    CoinGroupInfo coinGroup = coinGroups[denomAndId];

    int coinId;
    int mintHeight = GetMintedCoinHeightAndId(pubCoin, denomination, coinId);

    assert(coinId == id);

    // Find accumulator value preceding mint operation
    CBlockIndex *mintBlock = (*chain)[mintHeight];
    CBlockIndex *block = mintBlock;
    libzerocoin::Accumulator accumulator(ZCParams, d);
    if (block != coinGroup.firstBlock) {
        do {
            block = block->pprev;
        } while (block->accumulatorChanges.count(denomAndId) == 0);
        accumulator = libzerocoin::Accumulator(ZCParams, block->accumulatorChanges[denomAndId].first, d);
    }

    // Now add to the accumulator every coin minted since that moment except pubCoin
    block = coinGroup.lastBlock;
    while(true) {
        if (block->nHeight <= maxHeight && block->mintedPubCoins.count(denomAndId) > 0) {
            vector<CBigNum> &pubCoins = block->mintedPubCoins[denomAndId];
            for (const CBigNum &coin: pubCoins) {
                if (block != mintBlock || coin != pubCoin)
                    accumulator += libzerocoin::PublicCoin(ZCParams, coin, d);
            }
        }
        if (block != mintBlock)
            block = block->pprev;
        else
            break;
    }

    return libzerocoin::AccumulatorWitness(ZCParams, accumulator, libzerocoin::PublicCoin(ZCParams, pubCoin, d));
}

int CZerocoinState::GetMintedCoinHeightAndId(const CBigNum &pubCoin, int denomination, int &id) {
    auto coins = mintedPubCoins.equal_range(pubCoin);
    auto coinIt = find_if(coins.first, coins.second,
                          [=](const decltype(mintedPubCoins)::value_type &v) { return v.second.denomination == denomination; });

    if (coinIt != mintedPubCoins.end()) {
        id = coinIt->second.id;
        return coinIt->second.nHeight;
    }
    else
        return -1;
}

void CZerocoinState::Reset() {
    coinGroups.clear();
    usedCoinSerials.clear();
    mintedPubCoins.clear();
    latestCoinIds.clear();
}

CZerocoinState *CZerocoinState::GetZerocoinState() {
    return &zerocoinState;
}
