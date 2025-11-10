//////////////////////////////////////////////////////////////////////////////////
// garbage_collection.c for Cosmos+ OpenSSD
// Copyright (c) 2017 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//                Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: Garbage Collector
// File Name: garbage_collection.c
//
// Version: v1.0.0
//
// Description:
//   - select a victim block
//   - collect valid pages to a free block
//   - erase a victim block to make a free block
//
// NOTE (2025-11): This implementation uses the Cost-Benefit GC victim selection
//   policy. Public function names (e.g., GetFromGcVictimList) and data
//   structures are intentionally preserved for compatibility with the rest of
//   the firmware.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include "xil_printf.h"
#include <assert.h>
#include <stdint.h>     // [CB] for fixed-width integers
#include "memory_map.h"

P_GC_VICTIM_MAP gcVictimMapPtr;

// -------------------------- GC Policy Selection ------------------------------
// Only the Cost-Benefit policy is supported; legacy GREEDY mode is removed.
// -----------------------------------------------------------------------------

// [CB] Lightweight logical time for block invalidation “age”
static unsigned int gcActivityTick;
// [CB] Timestamp of the last erase for each block (used for classical cost-benefit age)
static unsigned int gcLastEraseTick[USER_DIES][USER_BLOCKS_PER_DIE];

// Forward declarations (keep existing external interfaces)
static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo);
static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo);

// Existing external helpers from other modules (not modified)
// extern unsigned int Vorg2VsaTranslation(unsigned int dieNo, unsigned int blockNo, unsigned int pageNo);
extern void EraseBlock(unsigned int dieNo, unsigned int blockNo);
extern unsigned int GetFromFreeReqQ(void);
extern void SelectLowLevelReqQ(unsigned int reqSlotTag);
extern unsigned int AllocateTempDataBuf(unsigned int dieNo);
extern void UpdateTempDataBufEntryInfoBlockingReq(unsigned int entry, unsigned int reqSlotTag);
extern unsigned int FindFreeVirtualSliceForGc(unsigned int dieNo, unsigned int victimBlockNo);

// ----------------------------- Initialization --------------------------------
void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;
    unsigned int blockNo;

    // [CB] Initialize logical time for aging
    gcActivityTick = 0;

    gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }

        // [CB] Initialize last-erase tick of all blocks
        //각 die 내의 모든 블록에 대해 "마지막으로 블록을 삭제(erase)한 시점"의 논리적 타임스탬프를 0으로 초기화
        for (blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo++)
            gcLastEraseTick[dieNo][blockNo] = 0;
    }
}

// ----------------------------- Main GC routine -------------------------------
void GarbageCollection(unsigned int dieNo)
{
    unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;

    // [Policy] Victim selection is inside GetFromGcVictimList (name preserved)
    victimBlockNo = GetFromGcVictimList(dieNo);
    dieNoForGcCopy = dieNo;

    if (virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
    {
        for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            if (logicalSliceAddr != LSA_NONE)
                if (logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr) // valid data
                {
                    // ---------------------------- READ ----------------------------
                    reqSlotTag = GetFromFreeReqQ();
                    reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                    reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
                    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
                    reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
                    UpdateTempDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
                    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

                    SelectLowLevelReqQ(reqSlotTag);

                    // ---------------------------- WRITE ---------------------------
                    reqSlotTag = GetFromFreeReqQ();
                    reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                    reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
                    reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
                    reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
                    reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
                    UpdateTempDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
                    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = FindFreeVirtualSliceForGc(dieNoForGcCopy, victimBlockNo);

                    logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
                    virtualSliceMapPtr->virtualSlice[reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr].logicalSliceAddr = logicalSliceAddr;
                    
                    SelectLowLevelReqQ(reqSlotTag);
                }
        }
    }

    EraseBlock(dieNo, victimBlockNo);

    // [CB에서 추가한 라인] Victim block was reset by erase; record current tick as its new birth time.
    // erase 직후에 gcLastEraseTick을 현재 tick으로 설정하면 그 블록의 age가 0이 되어 점수가 낮아지고, 
    // 이 뒤에 즉시 다시 GC 대상으로 선택되는 것을 막음
    gcLastEraseTick[dieNo][victimBlockNo] = gcActivityTick;
}


// Greedy 구현은 보통 "리스트의 head 또는 tail을 바로 꺼내는 방식"으로 victim을 선택하기 때문에 
// 제거 후 블록의 next/prev를 별도로 지워야 할 필요가 적습니다.
// Cost-Benefit 구현은 모든 후보(리스트의 임의 요소)를 스캔해서 최적 블록(bestBlock)을 고른다. 
// 이 경우 임의 위치에서 블록을 안전하게 분리(detach)해야 하므로, 
// 분리 후 블록의 next/prev 포인터를 명확히 지우는 안전한 래퍼(DetachBlockFromGcList)가 추가된 것.
// --------------------------- GC list manipulation ----------------------------
// [CB에서 추가한 함수] Detach helper keeps the public API (SelectiveGetFromGcVictimList) but
// makes call-sites cleaner. After detachment, next/prev are cleared.
static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo)
{
    SelectiveGetFromGcVictimList(dieNo, blockNo);
    virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
    virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
}

// --------------------------- Cost-Benefit Scoring ----------------------------
// Keep computations lightweight for firmware:
//  - integers only (use 64-bit for safe intermediate multiply)
//  - add +1 guards to avoid zero-division & favor decisive differences
static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int invalidSlices = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;
    unsigned int validSlices   = USER_PAGES_PER_BLOCK - invalidSlices;
    unsigned int ageTicks      = gcActivityTick - gcLastEraseTick[dieNo][blockNo];
    uint64_t benefit           = (uint64_t)invalidSlices * (uint64_t)(ageTicks + 1) * (uint64_t)USER_PAGES_PER_BLOCK;
    uint64_t cost              = (uint64_t)(validSlices + 1);

    if (cost == 0 || benefit == 0)
        return (uint32_t)benefit;

    return (uint32_t)(benefit / cost);
}

// NOTE: Called whenever a block moves between invalid-count bins.
// We bump the activity tick iff invalidSliceCnt > 0 so “age since became dirty”
// is meaningful. This is a very cheap logical timestamp (no timers needed).
// 리스트에 넣는 동작은 greedy랑 완전히 동일(O(1) 연결).
// 단, CB 점수에 필요한 ‘나이(age)’를 추적해야 함.
// 그래서 invalidSliceCnt > 0일 때만 gcActivityTick++을 추가:
void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (invalidSliceCnt)    // 'age' 개념을 논리적 이벤트 카운터로 구현한 것
        gcActivityTick++; // [CB에서 추가한 라인들] advance logical time when any block accumulates invalid data

    if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
        virtualBlockMapPtr->block[dieNo][gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock].nextBlock = blockNo;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = blockNo;
    }
    else
    {
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = blockNo;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = blockNo;
    }
}

//모든 후보를 전수 스캔해 (invalid×age)/(valid+1) 최댓값 선택 + 임의 위치 제거” 
// 비교적 정확·공정, 성능은 후보 수에 비례.
// ----------------------- Cost-Benefit Victim Selection -----------------------
unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int bestBlock = BLOCK_FAIL;
    uint32_t bestScore = 0;
    int invalidSliceCnt;

    // -------------------- Victim selection (COST-BENEFIT) --------------------
    // Score ~ benefit / cost = (invalid pages * age) / (valid pages to move)
    //  - invalid↑, age↑ → stronger incentive to clean the block
    //  - valid↑         → higher migration cost, so score decreases
    for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--)
    {
        unsigned int blockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;
        while (blockNo != BLOCK_NONE)
        {
            unsigned int nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock; // save next before scoring
            uint32_t score = CalculateCostBenefitScore(dieNo, blockNo);
            if (score > bestScore)
            {
                bestScore = score;
                bestBlock = blockNo;
            }
            blockNo = nextBlock;
        }
    }

    if (bestBlock != BLOCK_FAIL)
    {
        DetachBlockFromGcList(dieNo, bestBlock);
    }
    else
    {
        assert(!"[WARNING] There are no free blocks. Abort terminate this ssd. [WARNING]");
    }

    return bestBlock;
}


void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int nextBlock, prevBlock, invalidSliceCnt;

    nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
    prevBlock = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;
    invalidSliceCnt = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;

    if ((nextBlock != BLOCK_NONE) && (prevBlock != BLOCK_NONE))
    {
        virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = nextBlock;
        virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = prevBlock;
    }
    else if ((nextBlock == BLOCK_NONE) && (prevBlock != BLOCK_NONE))
    {
        virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = prevBlock;
    }
    else if ((nextBlock != BLOCK_NONE) && (prevBlock == BLOCK_NONE))
    {
        virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = nextBlock;
    }
    else
    {
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
    }
}
