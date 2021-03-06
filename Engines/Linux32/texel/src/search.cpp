/*
    Texel - A UCI chess engine.
    Copyright (C) 2012-2013  Peter Österlund, peterosterlund2@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * search.cpp
 *
 *  Created on: Feb 25, 2012
 *      Author: petero
 */

#include "search.hpp"
#include "treeLogger.hpp"
#include "textio.hpp"
#include "util/logger.hpp"

#include <iostream>
#include <iomanip>
#include <cmath>
#include <limits>

using namespace SearchConst;
using namespace Logger;

const int UNKNOWN_SCORE = -32767; // Represents unknown static eval score

Search::Search(const Position& pos0, const std::vector<U64>& posHashList0,
               int posHashListSize0, SearchTables& st, ParallelData& pd0,
               const std::shared_ptr<SplitPoint>& rootSp,
               TreeLogger& logFile0)
    : eval(st.et), kt(st.kt), ht(st.ht), tt(st.tt), pd(pd0), threadNo(0), logFile(logFile0) {
    stopHandler = std::make_shared<DefaultStopHandler>(*this);
    spVec.push_back(rootSp);
    init(pos0, posHashList0, posHashListSize0);
}

void
Search::init(const Position& pos0, const std::vector<U64>& posHashList0,
             int posHashListSize0) {
    pos = pos0;
    posHashList = posHashList0;
    posHashListSize = posHashListSize0;
    posHashFirstNew = posHashListSize;
    initNodeStats();
    minTimeMillis = -1;
    maxTimeMillis = -1;
    searchNeedMoreTime = false;
    maxNodes = -1;
    nodesBetweenTimeCheck = 10000;
    strength = 1000;
    weak = false;
    randomSeed = 0;
    tLastStats = currentTimeMillis();
    totalNodes = 0;
    nodesToGo = 0;
    verbose = false;
}

void
Search::timeLimit(int minTimeLimit, int maxTimeLimit) {
    minTimeMillis = minTimeLimit;
    maxTimeMillis = maxTimeLimit;
    if ((maxTimeMillis >= 0) && (maxTimeMillis < 1000))
        nodesBetweenTimeCheck = 1000;
    else
        nodesBetweenTimeCheck = 10000;
}

void
Search::setStrength(int strength, U64 randomSeed) {
    if (strength < 0) strength = 0;
    if (strength > 1000) strength = 1000;
    this->strength = strength;
    weak = strength < 1000;
    this->randomSeed = randomSeed;
}

Move
Search::iterativeDeepening(const MoveGen::MoveList& scMovesIn,
                           int maxDepth, U64 initialMaxNodes,
                           bool verbose, int maxPV) {
    tStart = currentTimeMillis();
    totalNodes = 0;
    if (scMovesIn.size <= 0)
        return Move(); // No moves to search

    std::vector<MoveInfo> scMoves;
    {
        // If strength is < 10%, only include a subset of the root moves.
        // At least one move is always included though.
        std::vector<bool> includedMoves(scMovesIn.size);
        U64 rndL = pos.zobristHash() ^ randomSeed;
        includedMoves[(int)(rndL % scMovesIn.size)] = true;
        double pIncl = (strength < 100) ? strength * strength * 1e-4 : 1.0;
        for (int mi = 0; mi < scMovesIn.size; mi++) {
            rndL = 6364136223846793005ULL * rndL + 1442695040888963407ULL;
            double rnd = ((rndL & 0x7fffffffffffffffULL) % 1000000000) / 1e9;
            if (!includedMoves[mi] && (rnd < pIncl))
                includedMoves[mi] = true;
        }
        for (int mi = 0; mi < scMovesIn.size; mi++) {
            if (includedMoves[mi]) {
                const Move& m = scMovesIn[mi];
                scMoves.push_back(MoveInfo(m, 0));
            }
        }
    }

    logFile.open("/home/petero/treelog.dmp", pd, threadNo);
    const U64 rootNodeIdx = logFile.logPosition(pos, 0, 0, 0);

    kt.clear();
    pd.npsInfo.reset();
//    pd.wq.resetStat();
    const bool smp = pd.numHelperThreads() > 0;
    maxNodes = initialMaxNodes;
    nodesToGo = 0;
    Position origPos(pos);
    bool firstIteration = true;
    Move bestMove = scMoves[0].move; // bestMove is != scMoves[0].move when there is an unresolved fail high
    this->verbose = verbose;
    if ((maxDepth < 0) || (maxDepth > MAX_SEARCH_DEPTH))
        maxDepth = MAX_SEARCH_DEPTH;
    maxPV = std::min(maxPV, (int)scMoves.size());
    for (size_t i = 0; i < COUNT_OF(searchTreeInfo); i++)
        searchTreeInfo[i].allowNullMove = true;
    ht.reScale();
    try {
    for (int depthS = plyScale; ; depthS += plyScale, firstIteration = false) {
        initNodeStats();
        if (listener) listener->notifyDepth(depthS/plyScale);
        int aspirationDelta = 0;
        int alpha = 0;
        UndoInfo ui;
        bool needMoreTime = false;
        for (int mi = 0; mi < (int)scMoves.size(); mi++) {
            if (mi < maxPV)
                aspirationDelta = (std::abs(scMoves[mi].score()) <= MATE0 / 2) ? aspirationWindow : 1000;
            if (firstIteration)
                alpha = -MATE0;
            else if (mi < maxPV)
                alpha = std::max(scMoves[mi].score() - aspirationDelta, -MATE0);
            else
                alpha = scMoves[maxPV-1].score();

            searchNeedMoreTime = (mi > 0);
            Move& m = scMoves[mi].move;
            pd.topMove = m;
            if (currentTimeMillis() - tStart >= 1000)
                if (listener) listener->notifyCurrMove(m, mi + 1);
            nodes = qNodes = 0;
            posHashList[posHashListSize++] = pos.zobristHash();
            bool givesCheck = MoveGen::givesCheck(pos, m);
            int beta;
            if (firstIteration)
                beta = MATE0;
            else if (mi < maxPV)
                beta = std::min(scMoves[mi].score() + aspirationDelta, MATE0);
            else
                beta = alpha + 1;

            int lmrS = 0;
            bool isCapture = (pos.getPiece(m.to()) != Piece::EMPTY);
            bool isPromotion = (m.promoteTo() != Piece::EMPTY);
            if ((depthS >= 3*plyScale) && !isCapture && !isPromotion &&
                !givesCheck && !passedPawnPush(pos, m) && (mi >= rootLMRMoveCount + maxPV)) {
                lmrS = plyScale;
            }
            pos.makeMove(m, ui);
            SearchTreeInfo& sti = searchTreeInfo[0];
            sti.currentMove = m;
            sti.currentMoveNo = mi;
            sti.lmr = lmrS;
            sti.nodeIdx = rootNodeIdx;
            int score = -negaScout(smp, -beta, -alpha, 1, depthS - lmrS - plyScale, -1, givesCheck);
            if ((lmrS > 0) && (score > alpha)) {
                sti.lmr = 0;
                score = -negaScout(smp, -beta, -alpha, 1, depthS - plyScale, -1, givesCheck);
            }
            U64 nodesThisMove = nodes + qNodes;
            posHashListSize--;
            pos.unMakeMove(m, ui);
            storeSearchResult(scMoves, mi, depthS, alpha, beta, score);
            if ((verbose && firstIteration) || (mi < maxPV) || (score > scMoves[maxPV-1].score()))
                notifyPV(scMoves, mi, maxPV);
            int betaRetryDelta = (mi == 0) ? aspirationDelta * 2 : aspirationDelta;
            int alphaRetryDelta = aspirationDelta * 2;
            while ((score >= beta) || ((mi < maxPV) && (score <= alpha))) {
                nodes = qNodes = 0;
                posHashList[posHashListSize++] = pos.zobristHash();
                bool fh = score >= beta;
                if (fh) {
                    if (score > MATE0 / 2)
                        betaRetryDelta = MATE0; // Don't use aspiration window when searching for faster mate
                    beta = std::min(score + betaRetryDelta, MATE0);
                    betaRetryDelta = betaRetryDelta * 3 / 2;
                    if (mi != 0)
                        needMoreTime = true;
                    bestMove = m;
                } else { // score <= alpha
                    if (score < -MATE0 / 2)
                        alphaRetryDelta = MATE0; // Don't use aspiration window when searching for faster mate
                    alpha = std::max(score - alphaRetryDelta, -MATE0);
                    alphaRetryDelta = alphaRetryDelta * 3 / 2;
                    needMoreTime = searchNeedMoreTime = true;
                }
                pos.makeMove(m, ui);
                score = -negaScout(smp, -beta, -alpha, 1, depthS - plyScale, -1, givesCheck);
                nodesThisMove += nodes + qNodes;
                posHashListSize--;
                pos.unMakeMove(m, ui);
                storeSearchResult(scMoves, mi, depthS, alpha, beta, score);
                notifyPV(scMoves, mi, maxPV);
            }
            scMoves[mi].nodes += nodesThisMove;
            if ((mi < maxPV) || (score > scMoves[maxPV-1].move.score())) {
                MoveInfo tmp = scMoves[mi];
                int i = mi;
                while ((i > 0) && (scMoves[i-1].score() < tmp.score())) {
                    scMoves[i] = scMoves[i-1];
                    i--;
                }
                scMoves[i] = tmp;
            }
            bestMove = scMoves[0].move;
            if (!firstIteration) {
                S64 timeLimit = needMoreTime ? maxTimeMillis : minTimeMillis;
                if (timeLimit >= 0) {
                    U64 tNow = currentTimeMillis();
                    if (tNow - tStart >= (U64)timeLimit)
                        break;
                }
            }
        }
        S64 tNow = currentTimeMillis();
        if (verbose) {
            static_assert(nodesByPly.minValue() == nodesByDepth.minValue(), "Incompatible histograms");
            static_assert(nodesByPly.maxValue() == nodesByDepth.maxValue(), "Incompatible histograms");
            for (int i = nodesByPly.minValue(); i < nodesByPly.maxValue(); i++)
                std::cout << std::setw(2) << i
                          << ' ' << std::setw(7) << nodesByPly.get(i)
                          << ' ' << std::setw(7) << nodesByDepth.get(i)
                          << std::endl;
            std::stringstream ss;
            ss.precision(3);
            ss << std::fixed << "Time: " << ((tNow - tStart) * .001);
            ss.precision(2);
            ss << " depth:" << (depthS/(double)plyScale)
               << " nps:" << ((int)(getTotalNodes() / ((tNow - tStart) * .001)));
            std::cout << ss.str() << std::endl;
        }
        if (maxTimeMillis >= 0)
            if (tNow - tStart >= minTimeMillis)
                break;
        if (depthS >= maxDepth * plyScale)
            break;
        if (maxNodes >= 0)
            if (getTotalNodes() >= maxNodes)
                break;
        bool enoughDepth = true;
        for (int i = 0; i < maxPV; i++) {
            int plyToMate = MATE0 - std::abs(scMoves[i].score());
            if (depthS < plyToMate * plyScale)
                enoughDepth = false;
        }
        if (enoughDepth)
            break;
        if (tNow > tStart)
            pd.npsInfo.setBaseNps(getTotalNodesThisThread() * 1000.0 / (tNow - tStart));
        if (firstIteration) {
            std::stable_sort(scMoves.begin()+maxPV, scMoves.end(), MoveInfo::SortByScore());
        } else {
            // Moves that were hard to search should be searched early in the next iteration
            std::stable_sort(scMoves.begin()+maxPV, scMoves.end(), MoveInfo::SortByNodes());
        }
//        std::cout << "fhInfo depth:" << depthS / plyScale << std::endl;
//        pd.fhInfo.print(std::cout);
//        std::cout << "wqStats depth:" << depthS / plyScale << std::endl;
//        pd.wq.printStats(std::cout, pd.numHelperThreads() + 1);
//        log([&](std::ostream& os){pd.npsInfo.print(os, depthS / plyScale);});
    }
    } catch (const StopSearch&) {
        pos = origPos;
    }
    notifyStats();

    logFile.close();
    return bestMove;
}

void
Search::storeSearchResult(std::vector<MoveInfo>& scMoves, int mi, int depth,
                          int alpha, int beta, int score) {
//    std::cout << "d:" << depth/plyScale << " mi:" << mi << " a:" << alpha
//              << " b:" << beta << " s:" << score << std::endl;
    scMoves[mi].depth = depth;
    scMoves[mi].alpha = alpha;
    scMoves[mi].beta = beta;
    scMoves[mi].move.setScore(score);
    scMoves[mi].pv.clear();
    tt.extractPVMoves(pos, scMoves[mi].move, scMoves[mi].pv);
}

void
Search::notifyPV(const std::vector<MoveInfo>& moveInfo, int mi, int maxPV) {
    bool miNotified = false;
    int lastReportedDepth = -1;
    for (int i = 0, n = 0; n < maxPV; i++) {
        if (!miNotified && (moveInfo[mi].score() > moveInfo[i].score()) &&
            (moveInfo[mi].score() > moveInfo[mi].alpha)) {
            notifyPV(moveInfo[mi], maxPV > 1 ? n : -1);
            lastReportedDepth = moveInfo[mi].depth;
            miNotified = true;
            n++;
            if (n >= maxPV)
                break;
        }
        if (i == mi) {
            if (!miNotified) {
                notifyPV(moveInfo[mi], maxPV > 1 ? n : -1);
                lastReportedDepth = moveInfo[mi].depth;
                miNotified = true;
                n++;
            }
        } else {
            notifyPV(moveInfo[i], maxPV > 1 ? n : -1);
            lastReportedDepth = moveInfo[i].depth;
            n++;
        }
    }
    if (listener && (moveInfo[mi].depth != lastReportedDepth))
        listener->notifyDepth(moveInfo[mi].depth/plyScale);
}

void
Search::notifyPV(const MoveInfo& info, int multiPVIndex) {
    if (info.depth <= 0)
        return;
    bool uBound = info.score() <= info.alpha;
    bool lBound = info.score() >= info.beta;
    int score = info.move.score();
    if (verbose) {
        std::stringstream ss;
        ss << std::setw(6) << std::left << TextIO::moveToString(pos, info.move, false)
           << ' ' << std::setw(6) << std::right << score
           << ' ' << std::setw(6) << nodes
           << ' ' << std::setw(6) << qNodes;
        if (uBound)
            ss << " <=";
        else if (lBound)
            ss << " >=";
        else {
            std::string PV = TextIO::moveToString(pos, info.move, false) + " ";
            UndoInfo ui;
            pos.makeMove(info.move, ui);
            PV += tt.extractPV(pos);
            pos.unMakeMove(info.move, ui);
            ss << ' ' << PV;
        }
        std::cout << ss.str() << std::endl;
    }
    if (!listener)
        return;
    bool isMate = false;
    if (score > MATE0 / 2) {
        isMate = true;
        score = (MATE0 - score) / 2;
    } else if (score < -MATE0 / 2) {
        isMate = true;
        score = -((MATE0 + score - 1) / 2);
    }
    U64 tNow = currentTimeMillis();
    int time = (int) (tNow - tStart);
    S64 totNodes = getTotalNodes();
    int nps = (time > 0) ? (int)(totNodes / (time / 1000.0)) : 0;
    listener->notifyPV(info.depth/plyScale, score, time, totNodes, nps, isMate,
                       uBound, lBound, info.pv, multiPVIndex);
}

void
Search::notifyStats() {
    S64 tNow = currentTimeMillis();
    if (listener) {
        int time = (int) (tNow - tStart);
        S64 totNodes = getTotalNodes();
        int nps = (time > 0) ? (int)(totNodes / (time / 1000.0)) : 0;
        listener->notifyStats(totNodes, nps, time);
    }
    tLastStats = tNow;
}

bool
Search::shouldStop() {
    S64 tNow = currentTimeMillis();
    S64 timeLimit = searchNeedMoreTime ? maxTimeMillis : minTimeMillis;
    if (    ((timeLimit >= 0) && (tNow - tStart >= timeLimit)) ||
            ((maxNodes >= 0) && (getTotalNodes() >= maxNodes)))
        return true;
    if (tNow - tLastStats >= 1000)
        notifyStats();
    return false;
}

template <bool smp>
int
Search::negaScout(int alpha, int beta, int ply, int depth, int recaptureSquare,
                  const bool inCheck) {
    typedef typename SplitPointTraits<smp>::SpHolder SplitPointHolder;

    // Mate distance pruning
    beta = std::min(beta, MATE0-ply-1);
    if (alpha >= beta)
        return alpha;

    if (logFile.isOpened()) {
        const SearchTreeInfo& sti = searchTreeInfo[ply-1];
        U64 idx = logFile.logNodeStart(sti.nodeIdx, sti.currentMove, alpha, beta, ply, depth);
        searchTreeInfo[ply].nodeIdx = idx;
    }
    if (nodesToGo <= 0) {
        nodesToGo = nodesBetweenTimeCheck;
        if (stopHandler->shouldStop())
            throw StopSearch();
    }

    // Collect statistics
    if (verbose) {
        nodesByPly.add(ply);
        nodesByDepth.add(depth/plyScale);
    }
    const U64 hKey = pos.historyHash();
    SearchTreeInfo& sti = searchTreeInfo[ply];
    sti.currentMove = emptyMove;
    sti.currentMoveNo = -1;

    // Draw tests
    if (canClaimDraw50(pos)) {
        if (inCheck) {
            MoveGen::MoveList moves;
            MoveGen::pseudoLegalMoves(pos, moves);
            MoveGen::removeIllegal(pos, moves);
            if (moves.size == 0) {            // Can't claim draw if already check mated.
                int score = -(MATE0-(ply+1));
                logFile.logNodeEnd(searchTreeInfo[ply].nodeIdx, score, TType::T_EXACT, UNKNOWN_SCORE, hKey);
                return score;
            }
        }
        logFile.logNodeEnd(searchTreeInfo[ply].nodeIdx, 0, TType::T_EXACT, UNKNOWN_SCORE, hKey);
        return 0;
    }
    if (canClaimDrawRep(pos, posHashList, posHashListSize, posHashFirstNew)) {
        logFile.logNodeEnd(searchTreeInfo[ply].nodeIdx, 0, TType::T_EXACT, UNKNOWN_SCORE, hKey);
        return 0;            // No need to test for mate here, since it would have been
                             // discovered the first time the position came up.
    }

    int evalScore = UNKNOWN_SCORE;
    // Check transposition table
    TranspositionTable::TTEntry ent;
    ent.clear();
    const bool useTT = (threadNo < 8) || (depth >= 1 * plyScale); // To reduce memory bandwidth
    if (useTT) tt.probe(hKey, ent);
    Move hashMove;
    if (ent.getType() != TType::T_EMPTY) {
        int score = ent.getScore(ply);
        evalScore = ent.getEvalScore();
        int plyToMate = MATE0 - std::abs(score);
        int eDepth = ent.getDepth();
        ent.getMove(hashMove);
        if (((beta == alpha + 1) || (depth <= ply*plyScale)) && ((eDepth >= depth) || (eDepth >= plyToMate*plyScale))) {
            if (     (ent.getType() == TType::T_EXACT) ||
                    ((ent.getType() == TType::T_GE) && (score >= beta)) ||
                    ((ent.getType() == TType::T_LE) && (score <= alpha))) {
                if (score >= beta) {
                    if (!hashMove.isEmpty())
                        if (pos.getPiece(hashMove.to()) == Piece::EMPTY)
                            kt.addKiller(ply, hashMove);
                }
                sti.bestMove = hashMove;
                logFile.logNodeEnd(sti.nodeIdx, score, ent.getType(), evalScore, hKey);
                return score;
            }
        }
    }

    int posExtend = inCheck ? plyScale : 0; // Check extension

    // If out of depth, perform quiescence search
    if (depth + posExtend <= 0) {
        q0Eval = evalScore;
        sti.bestMove.setMove(0,0,0,0);
        int score = quiesce(alpha, beta, ply, 0, inCheck);
        int type = TType::T_EXACT;
        if (score <= alpha) {
            type = TType::T_LE;
        } else if (score >= beta) {
            type = TType::T_GE;
        }
        sti.bestMove.setScore(score);
        if (useTT) tt.insert(hKey, sti.bestMove, type, ply, depth, q0Eval);
        logFile.logNodeEnd(sti.nodeIdx, score, type, q0Eval, hKey);
        return score;
    }

    // Razoring
    const bool normalBound = (alpha >= -MATE0 / 2) && (beta <= MATE0 / 2);
    if (normalBound && (depth < 4*plyScale) && (beta == alpha + 1)) {
        if (evalScore == UNKNOWN_SCORE) {
            evalScore = eval.evalPos(pos);
        }
        const int razorMargin = (depth <= plyScale) ? razorMargin1 : razorMargin2;
        if (evalScore < beta - razorMargin) {
            q0Eval = evalScore;
            int score = quiesce(alpha-razorMargin, beta-razorMargin, ply, 0, inCheck);
            if (score <= alpha-razorMargin) {
                emptyMove.setScore(score);
                if (useTT) tt.insert(hKey, emptyMove, TType::T_LE, ply, depth, q0Eval);
                logFile.logNodeEnd(sti.nodeIdx, score, TType::T_LE, q0Eval, hKey);
                return score;
            }
        }
    }

    // Reverse futility pruning
    if (!inCheck && (depth < 5*plyScale) && (posExtend == 0) && normalBound) {
        bool mtrlOk;
        if (pos.getWhiteMove()) {
            mtrlOk = (pos.wMtrl() > pos.wMtrlPawns()) && (pos.wMtrlPawns() > 0);
        } else {
            mtrlOk = (pos.bMtrl() > pos.bMtrlPawns()) && (pos.bMtrlPawns() > 0);
        }
        if (mtrlOk) {
            int margin;
            if (depth <= plyScale)        margin = reverseFutilityMargin1;
            else if (depth <= 2*plyScale) margin = reverseFutilityMargin2;
            else if (depth <= 3*plyScale) margin = reverseFutilityMargin3;
            else                          margin = reverseFutilityMargin4;
            if (evalScore == UNKNOWN_SCORE)
                evalScore = eval.evalPos(pos);
            if (evalScore - margin >= beta) {
                emptyMove.setScore(evalScore - margin);
                if (useTT) tt.insert(hKey, emptyMove, TType::T_GE, ply, depth, evalScore);
                logFile.logNodeEnd(sti.nodeIdx, evalScore - margin, TType::T_GE, evalScore, hKey);
                return evalScore - margin;
            }
        }
    }

    // Try null-move pruning
    if (    (depth >= 3*plyScale) && !inCheck && sti.allowNullMove &&
            (std::abs(beta) <= MATE0 / 2)) {
        bool nullOk;
        if (pos.getWhiteMove()) {
            nullOk = (pos.wMtrl() > pos.wMtrlPawns()) && (pos.wMtrlPawns() > 0);
        } else {
            nullOk = (pos.bMtrl() > pos.bMtrlPawns()) && (pos.bMtrlPawns() > 0);
        }
        const int R = (depth > 6*plyScale) ? 4*plyScale : 3*plyScale;
        if (nullOk) {
            if (((ent.getType() == TType::T_EXACT) || (ent.getType() == TType::T_LE)) &&
                (ent.getDepth() >= depth - R) && (ent.getScore(ply) < beta))
                nullOk = false;
        }
        if (nullOk) {
            if (evalScore == UNKNOWN_SCORE)
                evalScore = eval.evalPos(pos);
            if (evalScore < beta)
                nullOk = false;
        }
        if (nullOk) {
            int score;
            {
                SplitPointHolder sph(pd, spVec, pending);
                if (smp) {
                    sph.setSp(std::make_shared<SplitPoint>(threadNo, spVec.back(),
                                                           searchTreeInfo[ply-1].currentMoveNo,
                                                           pos, posHashList, posHashListSize,
                                                           sti, kt, ht, alpha, beta, ply, depth / plyScale));
                    sph.addMove(0, SplitPointMove(Move(), 0, 0, -1, false));
                    sph.addToQueue();
                    sph.setOwnerCurrMove(0, alpha);
                }
                pos.setWhiteMove(!pos.getWhiteMove());
                int epSquare = pos.getEpSquare();
                pos.setEpSquare(-1);
                searchTreeInfo[ply+1].allowNullMove = false;
                searchTreeInfo[ply+1].bestMove.setMove(0,0,0,0);
                score = -negaScout(smp, -beta, -(beta - 1), ply + 1, depth - R, -1, false);
                searchTreeInfo[ply+1].allowNullMove = true;
                pos.setEpSquare(epSquare);
                pos.setWhiteMove(!pos.getWhiteMove());
            }
            bool storeInHash = true;
            if ((score >= beta) && (depth >= 10 * plyScale)) {
                // Null-move verification search
                SearchTreeInfo& sti2 = searchTreeInfo[ply-1];
                const Move savedMove = sti2.currentMove;
                const int savedMoveNo = sti2.currentMoveNo;
                const S64 savedNodeIdx2 = sti2.nodeIdx;
                sti2.currentMove = Move(1,1,0); // Represents "no move"
                sti2.currentMoveNo = -1;
                sti2.nodeIdx = sti.nodeIdx;
                const S64 savedNodeIdx = sti.nodeIdx;
                sti.allowNullMove = false;
                score = negaScout(smp, beta - 1, beta, ply, depth - R, recaptureSquare, inCheck);
                sti.allowNullMove = true;
                sti.nodeIdx = savedNodeIdx;
                sti2.currentMove = savedMove;
                sti2.currentMoveNo = savedMoveNo;
                sti2.nodeIdx = savedNodeIdx2;
                searchTreeInfo[ply+1].bestMove.setMove(0,0,0,0);
                storeInHash = false;
            }
            if (smp && (depth - R >= MIN_SMP_DEPTH * plyScale))
                pd.fhInfo.addData(-1, searchTreeInfo[ply+1].currentMoveNo, score < beta, false);
            if (score >= beta) {
                if (score > MATE0 / 2)
                    score = beta;
                emptyMove.setScore(score);
                if (storeInHash)
                    if (useTT) tt.insert(hKey, emptyMove, TType::T_GE, ply, depth, evalScore);
                logFile.logNodeEnd(sti.nodeIdx, score, TType::T_GE, evalScore, hKey);
                return score;
            }
        }
    }

    bool futilityPrune = false;
    int futilityScore = alpha;
    if (!inCheck && (depth < 5*plyScale) && (posExtend == 0) && normalBound) {
        int margin;
        if (depth <= plyScale)        margin = futilityMargin1;
        else if (depth <= 2*plyScale) margin = futilityMargin2;
        else if (depth <= 3*plyScale) margin = futilityMargin3;
        else                          margin = futilityMargin4;
        if (evalScore == UNKNOWN_SCORE)
            evalScore = eval.evalPos(pos);
        futilityScore = evalScore + margin;
        if (futilityScore <= alpha)
            futilityPrune = true;
    }

    if ((depth > 4*plyScale) && hashMove.isEmpty()) {
        bool isPv = beta > alpha + 1;
        if (isPv || (depth > 8 * plyScale)) {
            // No hash move. Try internal iterative deepening.

            SearchTreeInfo& sti2 = searchTreeInfo[ply-1];
            Move savedMove = sti2.currentMove;
            int savedMoveNo = sti2.currentMoveNo;
            S64 savedNodeIdx2 = sti2.nodeIdx;
            sti2.currentMove = Move(1,1,0); // Represents "no move"
            sti2.currentMoveNo = -1;
            sti2.nodeIdx = sti.nodeIdx;

            S64 savedNodeIdx = sti.nodeIdx;
            int newDepth = isPv ? depth  - 2 * plyScale : depth * 3 / 8;
            negaScout(smp, alpha, beta, ply, newDepth, -1, inCheck);
            sti.nodeIdx = savedNodeIdx;

            sti2.currentMove = savedMove;
            sti2.currentMoveNo = savedMoveNo;
            sti2.nodeIdx = savedNodeIdx2;

            tt.probe(hKey, ent);
            if (ent.getType() != TType::T_EMPTY)
                ent.getMove(hashMove);
        }
    }

    // Start searching move alternatives
    MoveGen::MoveList moves;
    if (inCheck)
        MoveGen::checkEvasions(pos, moves);
    else
        MoveGen::pseudoLegalMoves(pos, moves);
    bool seeDone = false;
    bool hashMoveSelected = true;
    if (hashMove.isEmpty() || !selectHashMove(moves, hashMove)) {
        scoreMoveList(moves, ply);
        seeDone = true;
        hashMoveSelected = false;
    }

    SplitPointHolder sph(pd, spVec, pending);
    if (smp) {
        sph.setSp(std::make_shared<SplitPoint>(threadNo, spVec.back(),
                                               searchTreeInfo[ply-1].currentMoveNo,
                                               pos, posHashList, posHashListSize,
                                               sti, kt, ht, alpha, beta, ply, depth/plyScale));
        for (int mi = 0; mi < moves.size; mi++) {
            if ((mi == 1) && !seeDone) {
                scoreMoveList(moves, ply, 1);
                seeDone = true;
            }
            if ((mi > 0) || !hashMoveSelected)
                selectBest(moves, mi);
        }
    }
    bool lmpOk;
    if (pos.getWhiteMove())
        lmpOk = (pos.wMtrl() > pos.wMtrlPawns()) && (pos.wMtrlPawns() > 0);
    else
        lmpOk = (pos.bMtrl() > pos.bMtrlPawns()) && (pos.bMtrlPawns() > 0);
    int moveCountLimit = 256;
    if (lmpOk) {
        if (depth <= plyScale)          moveCountLimit = lmpMoveCountLimit1;
        else if (depth <= 2 * plyScale) moveCountLimit = lmpMoveCountLimit2;
        else if (depth <= 3 * plyScale) moveCountLimit = lmpMoveCountLimit3;
        else if (depth <= 4 * plyScale) moveCountLimit = lmpMoveCountLimit4;
    }
    UndoInfo ui;
    for (int pass = (smp?0:1); pass < 2; pass++) {
        bool haveLegalMoves = false;
        const int illegalScore = -(MATE0-(ply+1));
        int b = beta;
        int bestScore = illegalScore;
        int bestMove = -1;
        int lmrCount = 0;
        for (int mi = 0; mi < moves.size; mi++) {
            if (!smp) {
                if ((mi == 1) && !seeDone) {
                    scoreMoveList(moves, ply, 1);
                    seeDone = true;
                }
                if ((mi < moveCountLimit) || (lmrCount <= lmrMoveCountLimit1))
                    if ((mi > 0) || !hashMoveSelected)
                        selectBest(moves, mi);
            }
            Move& m = moves[mi];
            int newCaptureSquare = -1;
            bool isCapture = (pos.getPiece(m.to()) != Piece::EMPTY);
            bool isPromotion = (m.promoteTo() != Piece::EMPTY);
            int sVal = std::numeric_limits<int>::min();
            bool mayReduce = (m.score() < 53) && (!isCapture || m.score() < 0) && !isPromotion;
            bool givesCheck = MoveGen::givesCheck(pos, m);
            bool doFutility = false;
            if (mayReduce && haveLegalMoves && !givesCheck && !passedPawnPush(pos, m)) {
                if (normalBound && (bestScore >= -MATE0 / 2)) {
                    if (lmpOk && (mi >= moveCountLimit))
                        continue; // Late move pruning
                }
                if (futilityPrune)
                    doFutility = true;
            }
            int score = illegalScore;
            if (doFutility) {
                score = futilityScore;
            } else {
                if (!MoveGen::isLegal(pos, m, inCheck))
                    continue;
                int moveExtend = 0;
                if (posExtend == 0) {
                    const int pV = ::pV;
                    if ((m.to() == recaptureSquare)) {
                        if (sVal == std::numeric_limits<int>::min()) sVal = SEE(m);
                        int tVal = ::pieceValue[pos.getPiece(m.to())];
                        if (sVal > tVal - pV / 2)
                            moveExtend = plyScale;
                    }
                    if ((moveExtend < plyScale) && isCapture && (pos.wMtrlPawns() + pos.bMtrlPawns() > pV)) {
                        // Extend if going into pawn endgame
                        int capVal = ::pieceValue[pos.getPiece(m.to())];
                        if (pos.getWhiteMove()) {
                            if ((pos.wMtrl() == pos.wMtrlPawns()) && (pos.bMtrl() - pos.bMtrlPawns() == capVal))
                                moveExtend = plyScale;
                        } else {
                            if ((pos.bMtrl() == pos.bMtrlPawns()) && (pos.wMtrl() - pos.wMtrlPawns() == capVal))
                                moveExtend = plyScale;
                        }
                    }
                }
                int extend = std::max(posExtend, moveExtend);
                int lmr = 0;
                if ((depth >= 3*plyScale) && mayReduce && (extend == 0)) {
                    if (!givesCheck && !passedPawnPush(pos, m)) {
                        lmrCount++;
                        if ((lmrCount > lmrMoveCountLimit2) && (depth > 5*plyScale) && !isCapture) {
                            lmr = 3*plyScale;
                        } else if ((lmrCount > lmrMoveCountLimit1) && (depth > 3*plyScale) && !isCapture) {
                            lmr = 2*plyScale;
                        } else {
                            lmr = 1*plyScale;
                        }
                    }
                }
                int newDepth = depth - plyScale + extend - lmr;
                if (isCapture && (givesCheck || (depth + extend) > plyScale)) {
                    // Compute recapture target square, but only if we are not going
                    // into q-search at the next ply.
                    int fVal = ::pieceValue[pos.getPiece(m.from())];
                    int tVal = ::pieceValue[pos.getPiece(m.to())];
                    const int pV = ::pV;
                    if (std::abs(tVal - fVal) < pV / 2) {    // "Equal" capture
                        sVal = SEE(m);
                        if (std::abs(sVal) < pV / 2)
                            newCaptureSquare = m.to();
                    }
                }
                if (pass == 0) {
                    sph.addMove(mi, SplitPointMove(m, lmr, newDepth, newCaptureSquare, givesCheck));
                } else {
                    posHashList[posHashListSize++] = pos.zobristHash();
                    pos.makeMove(m, ui);
                    tt.prefetch(pos.historyHash());
                    nodes++;
                    totalNodes++;
                    nodesToGo--;
                    sti.currentMove = m;
                    sti.currentMoveNo = mi;
                    sti.lmr = lmr;
//                    S64 n1 = totalNodes; int nomDepth = newDepth;
                    if (smp)
                        sph.setOwnerCurrMove(mi, alpha);
                    score = -negaScout(smp, -b, -alpha, ply + 1, newDepth, newCaptureSquare, givesCheck);
                    if (((lmr > 0) && (score > alpha)) ||
                        ((score > alpha) && (score < beta) && (b != beta))) {
                        sti.lmr = 0;
                        newDepth += lmr;
                        score = -negaScout(smp, -beta, -alpha, ply + 1, newDepth, newCaptureSquare, givesCheck);
                    }
                    if (smp) {
//                        if (sph.hasHelperThread())
//                            log([&](std::ostream& os){os << "main seqNo:" << sph.getSeqNo() << " ply:" << ply << " m:" << mi
//                                                         << " a:" << alpha << " b:" << beta << " s:" << score
//                                                         << " d:" << nomDepth/plyScale << " n:" << (totalNodes-n1);});
                        if (beta > alpha + 1) {
                            pd.fhInfo.addPvData(mi, score > alpha);
                        } else {
                            pd.fhInfo.addData(mi, searchTreeInfo[ply+1].currentMoveNo,
                                              score <= alpha, !sph.isAllNode());
                        }
                    }
                    posHashListSize--;
                    pos.unMakeMove(m, ui);
                }
            }
            if (pass == 1) {
                if (weak && haveLegalMoves)
                    if (weakPlaySkipMove(pos, m, ply))
                        score = illegalScore;
                m.setScore(score);

                if (score != illegalScore)
                    haveLegalMoves = true;
                bestScore = std::max(bestScore, score);
                if (score > alpha) {
                    alpha = score;
                    bestMove = mi;
                    sti.bestMove.setMove(m.from(), m.to(), m.promoteTo(), sti.bestMove.score());
                }
                if (alpha >= beta) {
                    if (pos.getPiece(m.to()) == Piece::EMPTY) {
                        kt.addKiller(ply, m);
                        ht.addSuccess(pos, m, depth/plyScale);
                        for (int mi2 = mi - 1; mi2 >= 0; mi2--) {
                            Move m2 = moves[mi2];
                            if (pos.getPiece(m2.to()) == Piece::EMPTY)
                                ht.addFail(pos, m2, depth/plyScale);
                        }
                    }
                    if (useTT) tt.insert(hKey, m, TType::T_GE, ply, depth, evalScore);
                    logFile.logNodeEnd(sti.nodeIdx, alpha, TType::T_GE, evalScore, hKey);
                    return alpha;
                }
                b = alpha + 1;
            }
        }
        if (pass == 0) {
            sph.addToQueue();
        } else {
            if (!haveLegalMoves && !inCheck) {
                emptyMove.setScore(0);
                if (useTT) tt.insert(hKey, emptyMove, TType::T_EXACT, ply, depth, evalScore);
                logFile.logNodeEnd(sti.nodeIdx, 0, TType::T_EXACT, evalScore, hKey);
                return 0;       // Stale-mate
            }
            if (bestMove >= 0) {
                if (useTT) tt.insert(hKey, moves[bestMove], TType::T_EXACT, ply, depth, evalScore);
                logFile.logNodeEnd(sti.nodeIdx, bestScore, TType::T_EXACT, evalScore, hKey);
            } else {
                emptyMove.setScore(bestScore);
                if (useTT) tt.insert(hKey, emptyMove, TType::T_LE, ply, depth, evalScore);
                logFile.logNodeEnd(sti.nodeIdx, bestScore, TType::T_LE, evalScore, hKey);
            }
            return bestScore;
        }
    }
    assert(false);
    return 0;
}

bool
Search::weakPlaySkipMove(const Position& pos, const Move& m, int ply) const {
    U64 rndL = pos.zobristHash() ^ Position::getHashKey(Piece::EMPTY, m.from()) ^
               Position::getHashKey(Piece::EMPTY, m.to()) ^ randomSeed;
    double rnd = ((rndL & 0x7fffffffffffffffULL) % 1000000000) / 1e9;

    double s = strength * 1e-3;
    double offs = (17 - 50 * s) / 3;
    double effPly = ply * Evaluate::interpolate(pos.wMtrl() + pos.bMtrl(), 0, 30, ::qV * 4, 100) * 1e-2;
    double t = effPly + offs;
    double p = 1/(1+exp(t)); // Probability to "see" move
    bool easyMove = ((pos.getPiece(m.to()) != Piece::EMPTY) ||
                     (ply < 2) || (searchTreeInfo[ply-2].currentMove.to() == m.from()));
    if (easyMove)
        p = 1-(1-p)*(1-p);
    if (rnd > p)
        return true;
    return false;
}

int
Search::quiesce(int alpha, int beta, int ply, int depth, const bool inCheck) {
    int score;
    if (inCheck) {
        score = -(MATE0 - (ply+1));
    } else {
        if ((depth == 0) && (q0Eval != UNKNOWN_SCORE)) {
            score = q0Eval;
        } else {
            score = eval.evalPos(pos);
            if (depth == 0)
                q0Eval = score;
        }
    }
    if (score >= beta)
        return score;
    const int evalScore = score;
    if (score > alpha)
        alpha = score;
    int bestScore = score;
    const bool tryChecks = (depth > -1);
    MoveGen::MoveList moves;
    if (inCheck) {
        MoveGen::checkEvasions(pos, moves);
    } else if (tryChecks) {
        MoveGen::pseudoLegalCapturesAndChecks(pos, moves);
    } else {
        MoveGen::pseudoLegalCaptures(pos, moves);
    }

    bool realInCheckComputed = false;
    bool realInCheck = false;
    if (depth > -2) {
        realInCheckComputed = true;
        realInCheck = inCheck;
    }
    scoreMoveListMvvLva(moves);
    UndoInfo ui;
    for (int mi = 0; mi < moves.size; mi++) {
        if (mi < quiesceMaxSortMoves) {
            // If the first N moves didn't fail high this is probably an ALL-node,
            // so spending more effort on move ordering is probably wasted time.
            selectBest(moves, mi);
        }
        const Move& m = moves[mi];
        bool givesCheck = false;
        bool givesCheckComputed = false;
        if (inCheck) {
            // Allow all moves
        } else {
            if ((pos.getPiece(m.to()) == Piece::EMPTY) && (m.promoteTo() == Piece::EMPTY)) {
                // Non-capture
                if (!tryChecks)
                    continue;
                givesCheck = MoveGen::givesCheck(pos, m);
                givesCheckComputed = true;
                if (!givesCheck)
                    continue;
                if (negSEE(m)) // Needed because m.score() is not computed for non-captures
                    continue;
            } else {
                if (negSEE(m))
                    continue;
                int capt = ::pieceValue[pos.getPiece(m.to())];
                int prom = ::pieceValue[m.promoteTo()];
                int optimisticScore = evalScore + capt + prom + deltaPruningMargin;
                if (optimisticScore < alpha) { // Delta pruning
                    if ((pos.wMtrlPawns() > 0) && (pos.wMtrl() > capt + pos.wMtrlPawns()) &&
                        (pos.bMtrlPawns() > 0) && (pos.bMtrl() > capt + pos.bMtrlPawns())) {
                        if (depth -1 > -2) {
                            givesCheck = MoveGen::givesCheck(pos, m);
                            givesCheckComputed = true;
                        }
                        if (!givesCheck) {
                            if (optimisticScore > bestScore)
                                bestScore = optimisticScore;
                            continue;
                        }
                    }
                }
            }
        }
        if (!realInCheckComputed) {
            realInCheck = MoveGen::inCheck(pos);
            realInCheckComputed = true;
        }
        if (!MoveGen::isLegal(pos, m, realInCheck))
            continue;

        if (!givesCheckComputed && (depth - 1 > -2))
            givesCheck = MoveGen::givesCheck(pos, m);
        const bool nextInCheck = (depth - 1) > -2 ? givesCheck : false;

        pos.makeMove(m, ui);
        qNodes++;
        totalNodes++;
        nodesToGo--;
        score = -quiesce(-beta, -alpha, ply + 1, depth - 1, nextInCheck);
        pos.unMakeMove(m, ui);
        if (score > bestScore) {
            bestScore = score;
            if (score > alpha) {
                if (depth == 0) {
                    SearchTreeInfo& sti = searchTreeInfo[ply];
                    sti.bestMove.setMove(m.from(), m.to(), m.promoteTo(), score);
                }
                alpha = score;
                if (alpha >= beta)
                    return alpha;
            }
        }
    }
    return bestScore;
}


int
Search::SEE(const Move& m) {
    int captures[64];   // Value of captured pieces

    const int kV = ::kV;

    const int square = m.to();
    if (square == pos.getEpSquare()) {
        captures[0] = ::pV;
    } else {
        captures[0] = ::pieceValue[pos.getPiece(square)];
        if (captures[0] == kV)
            return kV;
    }
    int nCapt = 1;                  // Number of entries in captures[]

    UndoInfo ui;
    pos.makeSEEMove(m, ui);
    bool white = pos.getWhiteMove();
    int valOnSquare = ::pieceValue[pos.getPiece(square)];
    U64 occupied = pos.occupiedBB();
    while (true) {
        int bestValue = std::numeric_limits<int>::max();
        U64 atk;
        if (white) {
            atk = BitBoard::bPawnAttacks[square] & pos.pieceTypeBB(Piece::WPAWN) & occupied;
            if (atk != 0) {
                bestValue = ::pV;
            } else {
                atk = BitBoard::knightAttacks[square] & pos.pieceTypeBB(Piece::WKNIGHT) & occupied;
                if (atk != 0) {
                    bestValue = ::nV;
                } else {
                    U64 bAtk = BitBoard::bishopAttacks(square, occupied) & occupied;
                    atk = bAtk & pos.pieceTypeBB(Piece::WBISHOP);
                    if (atk != 0) {
                        bestValue = ::bV;
                    } else {
                        U64 rAtk = BitBoard::rookAttacks(square, occupied) & occupied;
                        atk = rAtk & pos.pieceTypeBB(Piece::WROOK);
                        if (atk != 0) {
                            bestValue = ::rV;
                        } else {
                            atk = (bAtk | rAtk) & pos.pieceTypeBB(Piece::WQUEEN);
                            if (atk != 0) {
                                bestValue = ::qV;
                            } else {
                                atk = BitBoard::kingAttacks[square] & pos.pieceTypeBB(Piece::WKING) & occupied;
                                if (atk != 0) {
                                    bestValue = kV;
                                } else {
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            atk = BitBoard::wPawnAttacks[square] & pos.pieceTypeBB(Piece::BPAWN) & occupied;
            if (atk != 0) {
                bestValue = ::pV;
            } else {
                atk = BitBoard::knightAttacks[square] & pos.pieceTypeBB(Piece::BKNIGHT) & occupied;
                if (atk != 0) {
                    bestValue = ::nV;
                } else {
                    U64 bAtk = BitBoard::bishopAttacks(square, occupied) & occupied;
                    atk = bAtk & pos.pieceTypeBB(Piece::BBISHOP);
                    if (atk != 0) {
                        bestValue = ::bV;
                    } else {
                        U64 rAtk = BitBoard::rookAttacks(square, occupied) & occupied;
                        atk = rAtk & pos.pieceTypeBB(Piece::BROOK);
                        if (atk != 0) {
                            bestValue = ::rV;
                        } else {
                            atk = (bAtk | rAtk) & pos.pieceTypeBB(Piece::BQUEEN);
                            if (atk != 0) {
                                bestValue = ::qV;
                            } else {
                                atk = BitBoard::kingAttacks[square] & pos.pieceTypeBB(Piece::BKING) & occupied;
                                if (atk != 0) {
                                    bestValue = kV;
                                } else {
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        captures[nCapt++] = valOnSquare;
        if (valOnSquare == kV)
            break;
        valOnSquare = bestValue;
        occupied &= ~(atk & -atk);
        white = !white;
    }
    pos.unMakeSEEMove(m, ui);

    int score = 0;
    for (int i = nCapt - 1; i > 0; i--)
        score = std::max(0, captures[i] - score);
    return captures[0] - score;
}

void
Search::scoreMoveList(MoveGen::MoveList& moves, int ply, int startIdx) {
    for (int i = startIdx; i < moves.size; i++) {
        Move& m = moves[i];
        bool isCapture = (pos.getPiece(m.to()) != Piece::EMPTY) || (m.promoteTo() != Piece::EMPTY);
        int score = 0;
        if (isCapture) {
            int seeScore = signSEE(m);
            int v = pos.getPiece(m.to());
            int a = pos.getPiece(m.from());
            score = Evaluate::pieceValueOrder[v] * 8 - Evaluate::pieceValueOrder[a];
            if (seeScore > 0)
                score += 100;
            else if (seeScore == 0)
                score += 50;
            else
                score -= 50;
            score *= 100;
        }
        int ks = kt.getKillerScore(ply, m);
        if (ks > 0) {
            score += ks + 50;
        } else {
            int hs = ht.getHistScore(pos, m);
            score += hs;
        }
        m.setScore(score);
    }
}

bool
Search::selectHashMove(MoveGen::MoveList& moves, const Move& hashMove) {
    for (int i = 0; i < moves.size; i++) {
        Move& m = moves[i];
        if (m.equals(hashMove)) {
            m.setScore(10000);
            std::swap(moves[i], moves[0]);
            return true;
        }
    }
    return false;
}

void
Search::initNodeStats() {
    nodes = qNodes = 0;
    nodesByPly.clear();
    nodesByDepth.clear();
}
