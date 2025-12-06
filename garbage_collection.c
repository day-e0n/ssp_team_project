//////////////////////////////////////////////////////////////////////////////////
// garbage_collection.c for Cosmos+ OpenSSD
// Copyright (c) 2017 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
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
//   - GameGC & Cost-Benefit GC integrated version
//   - select a victim block
//   - collect valid pages to a free block
//   - erase a victim block to make a free block
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

// #define ORIGINAL_GC // wdy: original GC 활성화
// #define ORIGINAL_GC // wdy: GAME GC 활성화
#define CB_GC // wdy: Cost_Benefit GC 활성화
//#define CBGAME_GC // wdy: Cost_Benefit + GAME GC 활성화

#if defined(ORIGINAL_GC)

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"

P_GC_VICTIM_MAP gcVictimMapPtr;

static int gcActive[USER_DIES] = {0};
static const unsigned int GC_LOW  = 512;
static const unsigned int GC_HIGH = 612;

// ===============================
// ORIGINAL GC SCHEDULER
// ===============================
void CheckAndRunOriginalGc(void)
{
    for (int die = 0; die < USER_DIES; die++)
    {
        unsigned int freeCnt = virtualDieMapPtr->die[die].freeBlockCnt;

        // GC 시작 조건
        if (!gcActive[die] && freeCnt <= GC_LOW)
        {
            gcActive[die] = 1;
            xil_printf("[ORIG_GC][ON ] Die %d GC ON  (free=%u)\r\n", die, freeCnt);
        }

        // GC 종료 조건
        if (gcActive[die] && freeCnt >= GC_HIGH)
        {
            gcActive[die] = 0;
            xil_printf("[ORIG_GC][OFF] Die %d GC OFF (free=%u)\r\n", die, freeCnt);
        }

        // GC 활성 구간에서 계속 수행
        if (gcActive[die])
            GarbageCollection(die);
    }
}

// ===============================
// GC 초기화
// ===============================
void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;

    gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }
    }
    xil_printf("[ORIG_GC] Init victim map completed\r\n");
}

// ===============================
// ORIGINAL GC CORE
// ===============================
void GarbageCollection(unsigned int dieNo)
{
    unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, reqSlotTag;

    victimBlockNo = GetFromGcVictimList(dieNo);

    if (victimBlockNo == BLOCK_FAIL || victimBlockNo == BLOCK_NONE)
    {
        xil_printf("[ORIG_GC][WARN] Die %d No victim block available\n", dieNo);
        return;
    }

    xil_printf("[ORIG_GC][VICTIM] Die %d Victim Block = %u (invalid=%u)\r\n",
               dieNo,
               victimBlockNo,
               virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt);

    // -------------------------------
    // Copy valid pages
    // -------------------------------
    if (virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
    {
        for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            // valid page
            if (logicalSliceAddr != LSA_NONE &&
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr)
            {
                xil_printf("[ORIG_GC][COPY] Die %d Page %u -> new location (LSA=%u)\r\n",
                           dieNo, pageNo, logicalSliceAddr);

                // READ
                reqSlotTag = GetFromFreeReqQ();
                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
                reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;
                SelectLowLevelReqQ(reqSlotTag);

                // WRITE
                reqSlotTag = GetFromFreeReqQ();
                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
                reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);

                unsigned int newVsa = FindFreeVirtualSliceForGc(dieNo, victimBlockNo);
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = newVsa;
                SelectLowLevelReqQ(reqSlotTag);

                // Update mapping
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = newVsa;
                virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr = logicalSliceAddr;
            }
        }
    }

    // -------------------------------
    // ERASE VICTIM BLOCK
    // -------------------------------
    xil_printf("[ORIG_GC][ERASE] Die %d Block %u erased\r\n", dieNo, victimBlockNo);
    EraseBlock(dieNo, victimBlockNo);
}

// ===============================
// Victim list functions
// ===============================
void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock =
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
        virtualBlockMapPtr->block[dieNo]
            [gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock].nextBlock = blockNo;
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

unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int evictedBlockNo;
    int invalidSliceCnt;

    for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--)
    {
        if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock != BLOCK_NONE)
        {
            evictedBlockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;

            xil_printf("[ORIG_GC][SELECT] Die %d pick invalidCnt=%d block=%u\r\n",
                       dieNo, invalidSliceCnt, evictedBlockNo);

            // remove node
            if (virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock != BLOCK_NONE)
            {
                virtualBlockMapPtr->block[dieNo]
                    [virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock].prevBlock = BLOCK_NONE;

                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock =
                    virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock;
            }
            else
            {
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
            }
            return evictedBlockNo;
        }
    }

    xil_printf("[ORIG_GC][WARN] Die %d no block found in victim list\r\n", dieNo);
    return BLOCK_FAIL;
}

#elif defined(GAME_GC)

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"
#include "garbage_collection.h"
#include "address_translation.h"

#define GC_SCHED_INTERVAL_TICK 100000
#define GC_PAGE_LIMIT 8
#define GC_TRIGGER_LOW   512     // GC 시작 임계값
#define GC_TRIGGER_HIGH  612     // GC 종료 임계값

P_GC_VICTIM_MAP gcVictimMapPtr;
INCREMENTAL_GC_CONTEXT gcCtx[USER_DIES];

int NeedGc(unsigned int dieNo)
{
    static unsigned int tick = 0;
    static int gcActive[USER_DIES] = {0};

    tick++;
    if (tick % 100000 == 0)
        xil_printf("[DBG] Die %d freeBlockCnt=%d\n",
                   dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);

    unsigned int freeCnt = virtualDieMapPtr->die[dieNo].freeBlockCnt;

    if (!gcActive[dieNo] && freeCnt <= GC_TRIGGER_LOW)
        gcActive[dieNo] = 1;
    else if (gcActive[dieNo] && freeCnt >= GC_TRIGGER_HIGH)
        gcActive[dieNo] = 0;

    return gcActive[dieNo];
}

void GcScheduler()
{
    static unsigned int tick = 0;
    static unsigned int logTick = 0;

    tick++;
    logTick++;

    if (tick % 1000 == 0)
    {
        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            if (gcCtx[dieNo].active)
                GarbageCollection(dieNo);
            else if (NeedGc(dieNo))
                GarbageCollection(dieNo);
        }
    }

    if (logTick % 100000 == 0)
    {
        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            xil_printf("[DBG][LOG] Die %d freeBlockCnt=%d, GC state=%d\r\n",
                       dieNo,
                       virtualDieMapPtr->die[dieNo].freeBlockCnt,
                       gcCtx[dieNo].state);
        }
    }
}

void TriggerGc(unsigned int dieNo)
{
    if (dieNo >= USER_DIES) return;

    INCREMENTAL_GC_CONTEXT *ctx = &gcCtx[dieNo];
    if (!ctx->active)
    {
        ctx->active = 1;
        ctx->state = GC_STATE_SELECT_VICTIM;
        xil_printf("[IGC] Triggered manually (Die %d)\r\n", dieNo);
    }
}

void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;
    gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        gcCtx[dieNo].state = GC_STATE_IDLE;
        gcCtx[dieNo].victimBlock = BLOCK_NONE;
        gcCtx[dieNo].curPage = 0;
        gcCtx[dieNo].active = 0;

        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }
    }
}

void GarbageCollection(unsigned int dieNo)
{
    INCREMENTAL_GC_CONTEXT *ctx = &gcCtx[dieNo];
    unsigned int pageNo, virtualSliceAddr, logicalSliceAddr;
    unsigned int reqSlotTag;

    switch (ctx->state)
    {
    case GC_STATE_IDLE:
        ctx->state = GC_STATE_SELECT_VICTIM;
        ctx->active = 1;
        xil_printf("[IGC] -> SELECT_VICTIM (Die %d)\r\n", dieNo);
        break;

    case GC_STATE_SELECT_VICTIM:
        ctx->victimBlock = GetFromGcVictimList(dieNo);
        if (ctx->victimBlock == BLOCK_NONE)
        {
            ctx->state = GC_STATE_IDLE;
            ctx->active = 0;
            return;
        }
        ctx->curPage = 0;
        ctx->state = GC_STATE_COPY_VALID_PAGES;
        xil_printf("[IGC] Victim selected (Die %d, Block %d)\r\n", dieNo, ctx->victimBlock);
        break;

    case GC_STATE_COPY_VALID_PAGES:
    {
        int copied = 0;
        while (copied < GC_PAGE_LIMIT && ctx->curPage < USER_PAGES_PER_BLOCK)
        {
            pageNo = ctx->curPage;
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, ctx->victimBlock, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            if (logicalSliceAddr != LSA_NONE &&
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr)
            {
                unsigned int newVsa = FindFreeVirtualSliceForGc(dieNo, ctx->victimBlock);
                unsigned int tempBuf = AllocateTempDataBuf(dieNo);

                reqSlotTag = GetFromFreeReqQ();
                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = tempBuf;
                SelectLowLevelReqQ(reqSlotTag);

                reqSlotTag = GetFromFreeReqQ();
                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = newVsa;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = tempBuf;
                SelectLowLevelReqQ(reqSlotTag);

                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = newVsa;
                virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr = logicalSliceAddr;
            }

            ctx->curPage++;
            copied++;
        }

        if (ctx->curPage >= USER_PAGES_PER_BLOCK)
            ctx->state = GC_STATE_ERASE_BLOCK;

        break;
    }

    case GC_STATE_ERASE_BLOCK:
        EraseBlock(dieNo, ctx->victimBlock);
        xil_printf("[IGC] Erased block (Die %d, Block %d)\r\n", dieNo, ctx->victimBlock);
        ctx->state = GC_STATE_IDLE;
        ctx->victimBlock = BLOCK_NONE;
        ctx->curPage = 0;
        ctx->active = 0;
        break;
    }
}

void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock =
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
        virtualBlockMapPtr->block[dieNo]
            [gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock].nextBlock = blockNo;
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

unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int evictedBlockNo;
    int invalidSliceCnt;

    for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--)
    {
        if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock != BLOCK_NONE)
        {
            evictedBlockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;

            if (virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock != BLOCK_NONE)
            {
                virtualBlockMapPtr->block[dieNo][virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock].prevBlock = BLOCK_NONE;
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock =
                    virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock;
            }
            else
            {
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
            }
            return evictedBlockNo;
        }
    }

    xil_printf("[IGC][WARN] No victim block available (Die %d, FreeBlocks=%d)\r\n",
               dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);
    return BLOCK_NONE;
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



#elif defined(CB_GC) // Cost_Benefit GC (Updated to Original-style low/high threshold GC)

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"
#include <stdint.h>

#define CB_GC_LOW   512     // freeBlockCnt ≤ 512 → GC ON
#define CB_GC_HIGH  612     // freeBlockCnt ≥ 612 → GC OFF

#define GC_DBG(...) xil_printf(__VA_ARGS__)

P_GC_VICTIM_MAP gcVictimMapPtr;

/* per-die GC active flag */
static int gcActive[USER_DIES] = {0};

/* global victim block (Original-style semantics) */
static unsigned int globalVictimBlock[USER_DIES];

/* CB GC age-tracking tick */
static unsigned int gcActivityTick = 0;

/* per-block last erase timestamp */
static unsigned int gcLastEraseTick[USER_DIES][USER_BLOCKS_PER_DIE];

/* GC debug counter */
static unsigned int gcCount = 0;

/* forward declarations */
static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo);
static void ValidatePostErase(unsigned int dieNo, unsigned int blockNo);


/* ============================
 *  ORIGINAL-STYLE GC SCHEDULER
 * ============================ */
void CheckAndRunOriginalGc(void)
{
    for (int die = 0; die < USER_DIES; die++)
    {
        unsigned int freeCnt = virtualDieMapPtr->die[die].freeBlockCnt;

        /* Turn ON GC */
        if (!gcActive[die] && freeCnt <= CB_GC_LOW)
        {
            gcActive[die] = 1;
            xil_printf("[CB_GC] Die %d GC ON (free=%u)\r\n", die, freeCnt);
        }

        /* Turn OFF GC */
        if (gcActive[die] && freeCnt >= CB_GC_HIGH)
        {
            gcActive[die] = 0;
            xil_printf("[CB_GC] Die %d GC OFF (free=%u)\r\n", die, freeCnt);
        }

        /* execute GC while active */
        if (gcActive[die])
            GarbageCollection(die);
    }
}


/* ============================
 *  INIT
 * ============================ */
void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt, blockNo;

    gcActivityTick = 0;
    gcVictimMapPtr = (P_GC_VICTIM_MAP)GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        gcActive[dieNo] = 0;
        globalVictimBlock[dieNo] = BLOCK_NONE;

        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }

        for (blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo++)
            gcLastEraseTick[dieNo][blockNo] = 0;
    }
}


/* ============================
 *  GC MAIN ROUTINE
 * ============================ */
void GarbageCollection(unsigned int dieNo)
{
    unsigned int victimBlockNo;
    unsigned int pageNo, virtualSliceAddr, logicalSliceAddr, reqSlotTag;
    unsigned int movedPages = 0;
    unsigned int movedLSA[USER_PAGES_PER_BLOCK];
    unsigned int movedVSA[USER_PAGES_PER_BLOCK];

    /* use global victim if preselected; else select new */
    victimBlockNo = globalVictimBlock[dieNo];

    if (victimBlockNo == BLOCK_NONE)
        victimBlockNo = GetFromGcVictimList(dieNo);

    if (victimBlockNo == BLOCK_FAIL || victimBlockNo == BLOCK_NONE)
        return;

    xil_printf("[CB_GC] Die %d victim=%d\r\n", dieNo, victimBlockNo);

    /* migrate valid pages */
    if (virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
    {
        for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            if (logicalSliceAddr != LSA_NONE &&
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr)
            {
                unsigned int tempBuf = AllocateTempDataBuf(dieNo);

                /* read */
                reqSlotTag = GetFromFreeReqQ();
                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = tempBuf;
                SelectLowLevelReqQ(reqSlotTag);

                /* write */
                reqSlotTag = GetFromFreeReqQ();
                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = tempBuf;
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr =
                    FindFreeVirtualSliceForGc(dieNo, victimBlockNo);

                /* update mapping */
                unsigned int newVsa = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = newVsa;
                virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr = logicalSliceAddr;

                movedLSA[movedPages] = logicalSliceAddr;
                movedVSA[movedPages] = newVsa;
                movedPages++;

                SelectLowLevelReqQ(reqSlotTag);
            }
        }
    }

    GC_DBG("[CB_GC] Moved %d pages die=%d block=%d\r\n",
           movedPages, dieNo, victimBlockNo);

    /* erase victim */
    EraseBlock(dieNo, victimBlockNo);
    gcLastEraseTick[dieNo][victimBlockNo] = gcActivityTick;

    xil_printf("[CB_GC] Erased die=%d block=%d\r\n", dieNo, victimBlockNo);

    globalVictimBlock[dieNo] = BLOCK_NONE;
    gcCount++;

    ValidatePostErase(dieNo, victimBlockNo);

    xil_printf("[CB_GC] GC completed die=%d block=%d\r\n", dieNo, victimBlockNo);
}


/* ============================
 *  COST-BENEFIT SELECTOR
 * ============================ */
unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int bestBlock = BLOCK_FAIL;
    uint32_t bestScore = 0;

    for (int isc = SLICES_PER_BLOCK; isc > 0; isc--)
    {
        unsigned int block = gcVictimMapPtr->gcVictimList[dieNo][isc].headBlock;
        while (block != BLOCK_NONE)
        {
            unsigned int next = virtualBlockMapPtr->block[dieNo][block].nextBlock;
            uint32_t score = CalculateCostBenefitScore(dieNo, block);

            if (score > bestScore)
            {
                bestScore = score;
                bestBlock = block;
            }
            block = next;
        }
    }

    if (bestBlock != BLOCK_FAIL)
    {
        /* detach from list */
        SelectiveGetFromGcVictimList(dieNo, bestBlock);

        unsigned int invalid = virtualBlockMapPtr->block[dieNo][bestBlock].invalidSliceCnt;
        unsigned int valid   = USER_PAGES_PER_BLOCK - invalid;
        unsigned int age     = gcActivityTick - gcLastEraseTick[dieNo][bestBlock];

        GC_DBG("[CB_GC] Victim die=%d block=%d score=%u invalid=%u valid=%u age=%u\r\n",
               dieNo, bestBlock, bestScore, invalid, valid, age);
    }
    else
    {
        xil_printf("[CB_GC][WARN] No victim available die=%d\r\n", dieNo);
    }

    return bestBlock;
}


/* ============================
 *  SCORING
 * ============================ */
static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int invalid = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;
    unsigned int valid   = USER_PAGES_PER_BLOCK - invalid;
    unsigned int age     = gcActivityTick - gcLastEraseTick[dieNo][blockNo];

    if (valid == 0) valid = 1;

    uint64_t benefit = (uint64_t)invalid * (uint64_t)(age + 1) * USER_PAGES_PER_BLOCK;
    uint64_t cost    = (uint64_t)valid;

    return (uint32_t)(benefit / cost);
}


/* ============================
 *  POST-ERASE VALIDATION
 * ============================ */
static void ValidatePostErase(unsigned int dieNo, unsigned int blockNo)
{
    if (virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt != 0)
        GC_DBG("[CB_GC][ERR] invalidSliceCnt not zero after erase die=%d block=%d\r\n",
               dieNo, blockNo);
}


/* ============================
 *  LIST INSERTION
 * ============================ */
void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (invalidSliceCnt > 0)
        gcActivityTick++;

    if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
    {
        unsigned int tail = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = tail;
        virtualBlockMapPtr->block[dieNo][tail].nextBlock = blockNo;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
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


/* ============================
 *  LIST REMOVAL
 * ============================ */
void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int next = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
    unsigned int prev = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;
    unsigned int isc  = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;

    if (next != BLOCK_NONE && prev != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][prev].nextBlock = next;
        virtualBlockMapPtr->block[dieNo][next].prevBlock = prev;
    }
    else if (next == BLOCK_NONE && prev != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][prev].nextBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][isc].tailBlock = prev;
    }
    else if (next != BLOCK_NONE && prev == BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][next].prevBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][isc].headBlock = next;
    }
    else
    {
        gcVictimMapPtr->gcVictimList[dieNo][isc].headBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][isc].tailBlock = BLOCK_NONE;
    }

    virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
    virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
}


#elif defined(CBGAME_GC)   // Cost-Benefit + Incremental GC (GameGC 방식)

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"
#include <stdint.h>
#include "address_translation.h"

#define CB_GC_LOW   512     // GC ON threshold
#define CB_GC_HIGH  612     // GC OFF threshold
#define GC_PAGE_LIMIT 8     // incremental copy per cycle

#define GC_DBG(...) xil_printf(__VA_ARGS__)

P_GC_VICTIM_MAP gcVictimMapPtr;

static unsigned int gcActivityTick;
static unsigned int gcLastEraseTick[USER_DIES][USER_BLOCKS_PER_DIE];
static unsigned int gcCount = 0;

static int gcActive[USER_DIES];    // <-- threshold ON/OFF 플래그

/* incremental state-machine */
typedef struct {
    unsigned int state;
    unsigned int victimBlock;
    unsigned int curPage;
    unsigned char active;
} CBGAME_GC_CTX;

static CBGAME_GC_CTX cbCtx[USER_DIES];

/*===========================================================================
 * Cost-Benefit score
 *===========================================================================*/
static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int invalid = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;
    unsigned int valid   = USER_PAGES_PER_BLOCK - invalid;
    unsigned int age     = gcActivityTick - gcLastEraseTick[dieNo][blockNo];

    uint64_t benefit = (uint64_t)invalid * (uint64_t)(age + 1) * USER_PAGES_PER_BLOCK;
    uint64_t cost    = (uint64_t)(valid + 1);

    return (uint32_t)(benefit / cost);
}

/*===========================================================================
 * Init
 *===========================================================================*/
void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;
    unsigned int blockNo;

    gcActivityTick = 0;
    gcVictimMapPtr = (P_GC_VICTIM_MAP)GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        gcActive[dieNo] = 0;

        cbCtx[dieNo].state = GC_STATE_IDLE;
        cbCtx[dieNo].victimBlock = BLOCK_NONE;
        cbCtx[dieNo].curPage = 0;
        cbCtx[dieNo].active = 0;

        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }

        for (blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo++)
            gcLastEraseTick[dieNo][blockNo] = 0;
    }
}

/*===========================================================================
 * Victim selection (Cost-Benefit)
 *===========================================================================*/
unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int best = BLOCK_FAIL;
    uint32_t bestScore = 0;

    for (int isc = SLICES_PER_BLOCK; isc > 0; isc--)
    {
        unsigned int blk = gcVictimMapPtr->gcVictimList[dieNo][isc].headBlock;

        while (blk != BLOCK_NONE)
        {
            unsigned int next = virtualBlockMapPtr->block[dieNo][blk].nextBlock;
            uint32_t score = CalculateCostBenefitScore(dieNo, blk);

            if (score > bestScore)
            {
                bestScore = score;
                best = blk;
            }
            blk = next;
        }
    }

    if (best == BLOCK_FAIL)
    {
        xil_printf("[CBGAME_GC][WARN] No victim block on die %d\r\n", dieNo);
        return BLOCK_FAIL;
    }

    SelectiveGetFromGcVictimList(dieNo, best);

    xil_printf("[CBGAME_GC] Victim die=%d block=%d score=%u\r\n",
               dieNo, best, bestScore);

    return best;
}

/*===========================================================================
 * PutToGcVictimList
 *===========================================================================*/
void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (invalidSliceCnt)
        gcActivityTick++;

    if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
    {
        unsigned int tail = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;

        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = tail;
        virtualBlockMapPtr->block[dieNo][tail].nextBlock = blockNo;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;

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

/*===========================================================================
 * Threshold-based Incremental Scheduler (GameGC 스타일 유지)
 *===========================================================================*/
void GcScheduler(void)
{
    static unsigned int tick = 0;
    tick++;

    if (tick % 1000 == 0)
    {
        for (int die = 0; die < USER_DIES; die++)
        {
            unsigned int freeCnt = virtualDieMapPtr->die[die].freeBlockCnt;

            /* ---- Threshold ON/OFF ---- */
            if (!gcActive[die] && freeCnt <= CB_GC_LOW)
            {
                gcActive[die] = 1;
                xil_printf("[CBGAME_GC] Die %d GC ON (free=%u)\r\n", die, freeCnt);
            }
            else if (gcActive[die] && freeCnt >= CB_GC_HIGH)
            {
                gcActive[die] = 0;
                xil_printf("[CBGAME_GC] Die %d GC OFF (free=%u)\r\n", die, freeCnt);
            }

            /* ---- Incremental GC 실행 ---- */
            if (gcActive[die])
                GarbageCollection(die);
        }
    }
}

/*===========================================================================
 * Incremental GC Body
 *===========================================================================*/
void GarbageCollection(unsigned int dieNo)
{
    CBGAME_GC_CTX *ctx = &cbCtx[dieNo];
    unsigned int pageNo, vsa, lsa;

    switch (ctx->state)
    {
    case GC_STATE_IDLE:
        ctx->active = 1;
        ctx->state = GC_STATE_SELECT_VICTIM;
        xil_printf("[CBGAME_GC] -> SELECT_VICTIM (die %d)\r\n", dieNo);
        break;

    case GC_STATE_SELECT_VICTIM:
        ctx->victimBlock = GetFromGcVictimList(dieNo);

        if (ctx->victimBlock == BLOCK_FAIL)
        {
            ctx->state = GC_STATE_IDLE;
            ctx->active = 0;
            return;
        }

        xil_printf("[CBGAME_GC] Start victim %d on die %d\r\n", ctx->victimBlock, dieNo);

        ctx->curPage = 0;
        ctx->state = GC_STATE_COPY_VALID_PAGES;
        break;

    case GC_STATE_COPY_VALID_PAGES:
    {
        int moved = 0;

        while (moved < GC_PAGE_LIMIT && ctx->curPage < USER_PAGES_PER_BLOCK)
        {
            pageNo = ctx->curPage++;
            vsa = Vorg2VsaTranslation(dieNo, ctx->victimBlock, pageNo);
            lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;

            if (lsa != LSA_NONE &&
                logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr == vsa)
            {
                unsigned int newVsa = FindFreeVirtualSliceForGc(dieNo, ctx->victimBlock);
                unsigned int temp = AllocateTempDataBuf(dieNo);

                unsigned int req = GetFromFreeReqQ();
                reqPoolPtr->reqPool[req].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[req].reqCode = REQ_CODE_READ;
                reqPoolPtr->reqPool[req].nandInfo.virtualSliceAddr = vsa;
                reqPoolPtr->reqPool[req].dataBufInfo.entry = temp;
                SelectLowLevelReqQ(req);

                req = GetFromFreeReqQ();
                reqPoolPtr->reqPool[req].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[req].reqCode = REQ_CODE_WRITE;
                reqPoolPtr->reqPool[req].nandInfo.virtualSliceAddr = newVsa;
                reqPoolPtr->reqPool[req].dataBufInfo.entry = temp;
                SelectLowLevelReqQ(req);

                logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = newVsa;
                virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr = lsa;

                moved++;
            }
        }

        if (ctx->curPage >= USER_PAGES_PER_BLOCK)
            ctx->state = GC_STATE_ERASE_BLOCK;

        break;
    }

    case GC_STATE_ERASE_BLOCK:

        EraseBlock(dieNo, ctx->victimBlock);
        xil_printf("[CBGAME_GC] Erased die=%d block=%d\r\n",
                   dieNo, ctx->victimBlock);

        gcLastEraseTick[dieNo][ctx->victimBlock] = gcActivityTick;
        gcCount++;

        ctx->victimBlock = BLOCK_NONE;
        ctx->curPage = 0;
        ctx->state = GC_STATE_IDLE;
        ctx->active = 0;

        xil_printf("[CBGAME_GC] GC Complete (die=%d)\r\n", dieNo);

        break;
    }
}

/*===========================================================================
 * Victim removal from list
 *===========================================================================*/
void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int next = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
    unsigned int prev = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;
    unsigned int invalid = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;

    if (next != BLOCK_NONE && prev != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][prev].nextBlock = next;
        virtualBlockMapPtr->block[dieNo][next].prevBlock = prev;
    }
    else if (next == BLOCK_NONE && prev != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][prev].nextBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalid].tailBlock = prev;
    }
    else if (next != BLOCK_NONE && prev == BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][next].prevBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalid].headBlock = next;
    }
    else
    {
        gcVictimMapPtr->gcVictimList[dieNo][invalid].headBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalid].tailBlock = BLOCK_NONE;
    }
}

#endif
