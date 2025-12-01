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

#if defined(ORIGINAL_GC)

#include "garbage_collection.h"
#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"

#define GC_TRIGGER_THRESHOLD 1990

P_GC_VICTIM_MAP gcVictimMapPtr;
unsigned int gcTriggered;
unsigned int copyCnt;
unsigned int globalVictimBlock[USER_DIES];

void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;

    gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        globalVictimBlock[dieNo] = BLOCK_NONE;

        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }
    }
}

void CheckAndRunStwGc(void)
{
    static unsigned int tick = 0;
    int needGc = 0;

    tick++;

    if (tick % 100000 == 0)
    {
        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            xil_printf("  Die %d: freeBlockCnt=%d\r\n",
                       dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);
        }
    }

    for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        if (virtualDieMapPtr->die[dieNo].freeBlockCnt <= GC_TRIGGER_THRESHOLD)
        {
            needGc = 1;
            break;
        }
    }

    if (needGc)
    {
        xil_printf("[STW_GC] Triggered\r\n");

        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            unsigned int victimBlock = GetFromGcVictimList(dieNo);

            if (victimBlock != BLOCK_NONE)
            {
                globalVictimBlock[dieNo] = victimBlock;
                xil_printf("[STW_GC] Die %d victim=%d\r\n", dieNo, victimBlock);
                GarbageCollection(dieNo);
            }
        }
    }
}

void GarbageCollection(unsigned int dieNo)
{
    unsigned int victimBlockNo = globalVictimBlock[dieNo];
    unsigned int pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;

    if (victimBlockNo == BLOCK_NONE)
        return;

    dieNoForGcCopy = dieNo;

    xil_printf("[STW_GC] GC start die=%d block=%d\r\n", dieNo, victimBlockNo);

    if (virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
    {
        for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            if (logicalSliceAddr != LSA_NONE &&
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr)
            {
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

                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr =
                    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
                virtualSliceMapPtr->virtualSlice[reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr].logicalSliceAddr =
                    logicalSliceAddr;

                SelectLowLevelReqQ(reqSlotTag);
            }
        }
    }

    EraseBlock(dieNo, victimBlockNo);
    xil_printf("[STW_GC] Erased die=%d block=%d\r\n", dieNo, victimBlockNo);
    globalVictimBlock[dieNo] = BLOCK_NONE;
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

    xil_printf("[STW_GC][WARN] No victim block available on die %d\r\n", dieNo);
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

#elif defined(GAME_GC)

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"
#include "garbage_collection.h"
#include "address_translation.h"

#define GC_SCHED_INTERVAL_TICK 100000
#define GC_PAGE_LIMIT 8
#define GC_TRIGGER_LOW   1900     // GC 시작 임계값
#define GC_TRIGGER_HIGH  2000     // GC 종료 임계값

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



#elif defined(CB_GC) // Cost_Benefit GC

	#include "xil_printf.h"
	#include <assert.h>
	#include "memory_map.h"
	#include <stdint.h>     // [CB] for fixed-width integers

    /* CB_GC trigger threshold (event-driven). Adjust as needed. */
    #define CB_GC_TRIGGER_THRESHOLD 1990

    #if 1
    // 디버그 출력을 항상 활성화: 매크로는 조건 없이 출력합니다.
    #define GC_DBG(...) xil_printf(__VA_ARGS__)
    #endif

	P_GC_VICTIM_MAP gcVictimMapPtr;

	// 외부 인터페이스와의 호환성을 위한 변수들
	unsigned int gcTriggered;
	unsigned int copyCnt;

    /* Per-die global victim holder to mirror ORIGINAL_GC semantics */
    unsigned int globalVictimBlock[USER_DIES];

	// -------------------------- GC 정책 선택 ------------------------------
	// Cost-Benefit 정책만 지원합니다. 레거시 GREEDY 모드는 제거되었습니다.
	// -----------------------------------------------------------------------------

	// [CB] 블록 무효화의 '나이'를 위한 경량 논리 시간
	static unsigned int gcActivityTick;
	// [CB] 블록별 마지막 erase 타임스탬프
	static unsigned int gcLastEraseTick[USER_DIES][USER_BLOCKS_PER_DIE];
	// GC 발생 횟수(진단용)
	static unsigned int gcCount = 0;

	// 전방 선언 (외부 인터페이스 유지)
	static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo);
	static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo);
    /* Forward declaration to avoid implicit non-static declaration when
     * `ValidatePostErase` is called before its static definition below.
     */
    static void ValidatePostErase(unsigned int dieNo, unsigned int blockNo);

	// 외부 모듈의 헬퍼 함수들 (수정 없음)
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

		gcActivityTick = 0;

		gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

		for (dieNo = 0; dieNo < USER_DIES; dieNo++)
		{
			for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
			{
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
			}

			/* 모든 블록의 마지막 erase 시각 초기화 */
			for (blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo++)
				gcLastEraseTick[dieNo][blockNo] = 0;

            /* initialize per-die global victim like ORIGINAL_GC */
            globalVictimBlock[dieNo] = BLOCK_NONE;
		}
	}

    /*
     * Provide a public CheckAndRunStwGc() symbol for builds that call
     * it unconditionally (e.g., in `nvme_main.c`). The CB_GC variant
     * implements a different GC policy; offer a small, non-invasive
     * implementation that advances the internal tick. This satisfies
     * the linker when the full STW GC routine is not compiled.
     */
    void CheckAndRunStwGc(void)
    {
        static unsigned int tick = 0;
        int needGc = 0;

        tick++;

        if (tick % 100000 == 0)
        {
            for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
            {
                xil_printf("  Die %d: freeBlockCnt=%d\r\n",
                           dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);
            }
        }

        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            if (virtualDieMapPtr->die[dieNo].freeBlockCnt <= CB_GC_TRIGGER_THRESHOLD)
            {
                needGc = 1;
                break;
            }
        }

        if (needGc)
        {
            xil_printf("[CB_GC] Triggered\r\n");

            for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
            {
                unsigned int victimBlock = GetFromGcVictimList(dieNo);

                if (victimBlock != BLOCK_FAIL)
                {
                    globalVictimBlock[dieNo] = victimBlock;
                    xil_printf("[CB_GC] Die %d victim=%d\r\n", dieNo, victimBlock);
                    GarbageCollection(dieNo);
                }
            }
        }
    }
		// ----------------------------- 메인 GC 루틴 -------------------------------
	void GarbageCollection(unsigned int dieNo)
	{
		unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;
		unsigned int movedPages = 0;
		unsigned int movedLogical[USER_PAGES_PER_BLOCK];
		unsigned int movedDestVsa[USER_PAGES_PER_BLOCK];

        /* prefer a globalVictimBlock if set by the scheduler (ORIGINAL style) */
        victimBlockNo = globalVictimBlock[dieNo];
        if (victimBlockNo == BLOCK_NONE || victimBlockNo == BLOCK_FAIL)
        {
            /* fallback: select via CB scoring */
            victimBlockNo = GetFromGcVictimList(dieNo);
            if (victimBlockNo == BLOCK_FAIL)
                return;
        }

        xil_printf("  Die %d: freeBlockCnt=%d\r\n",
                dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);
        xil_printf("[CB_GC] Triggered\r\n");

		xil_printf("[CB_GC] Die %d victim=%d\r\n", dieNo, victimBlockNo);
		xil_printf("[CB_GC] GC start die=%d block=%d\r\n", dieNo, victimBlockNo);
		dieNoForGcCopy = dieNo;

		/* 유효 페이지가 있으면 이동 수행 */
		if (virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
		{
			for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
			{
				virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
				logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;
				/* 유효 매핑 확인 */
				if (logicalSliceAddr != LSA_NONE)
					if (logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr)
					{
						/* READ 요청 */

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

						/* WRITE 요청 */
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

						/* GC를 위한 가상 슬라이스 할당 */
						reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = FindFreeVirtualSliceForGc(dieNoForGcCopy, victimBlockNo);

						/* 매핑 갱신 (논리->가상, 가상->논리) */
						logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
						virtualSliceMapPtr->virtualSlice[reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr].logicalSliceAddr = logicalSliceAddr;
						/* 이동된 페이지 기록 */
						movedLogical[movedPages] = logicalSliceAddr;
						movedDestVsa[movedPages] = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
						movedPages++;

						SelectLowLevelReqQ(reqSlotTag);
					}
			}
		}

		/* 이동된 페이지 매핑 검증 */
		{
			unsigned int i;
			for (i = 0; i < movedPages; i++)
			{
				unsigned int lsa = movedLogical[i];
				unsigned int vsa = movedDestVsa[i];
				if (logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr != vsa)
				{
					GC_DBG("[CB_GC][ERROR] Mapping mismatch after GC die=%d victim=%d lsa=%d expected_vsa=%d actual_vsa=%d\r\n",
							dieNo, victimBlockNo, lsa, vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
				}
			}
		}

		GC_DBG("[CB_GC][DBG] Moved %d pages during GC die=%d victim=%d\r\n", movedPages, dieNo, victimBlockNo);

		EraseBlock(dieNo, victimBlockNo);

        xil_printf("[CB_GC] Erased die=%d block=%d\r\n", dieNo, victimBlockNo);
        /* mirror ORIGINAL: clear the global victim pointer for this die */
        globalVictimBlock[dieNo] = BLOCK_NONE;

		/* 즉시 재선택 방지를 위해 erase 시각 기록 */
		gcLastEraseTick[dieNo][victimBlockNo] = gcActivityTick;
		gcCount++;
		GC_DBG("[CB_GC][DBG] Erase count die=%d block=%d total_gc_count=%d\r\n", dieNo, victimBlockNo, gcCount);
		/* erase 후 상태 검증 */
		ValidatePostErase(dieNo, victimBlockNo);

		/* 추가 디버깅 출력: GC 작업 완료 */
		if (movedPages > 0) {
			xil_printf("[CB_GC] Moved %d pages from die=%d block=%d\r\n", movedPages, dieNo, victimBlockNo);
		} else {
			xil_printf("[CB_GC][WARN] No valid pages to move from die=%d block=%d\r\n", dieNo, victimBlockNo);
		}

		/* 추가 디버깅 출력: GC 종료 */
		xil_printf("[CB_GC] GC completed for die=%d block=%d\r\n", dieNo, victimBlockNo);
	}
	// --------------------------- GC 리스트 조작 ----------------------------
	// Detach helper: remove block from list and clear next/prev links
	static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo)
	{
		SelectiveGetFromGcVictimList(dieNo, blockNo);
		virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
		virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
		GC_DBG("[CB_GC][DBG] Detached block die=%d block=%d\r\n", dieNo, blockNo);
	}

	// --------------------------- Cost-Benefit Scoring ----------------------------
	// Integer arithmetic; +1 guards avoid divide-by-zero
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

	// Debug helper: print block stats and score
	static void DumpGcStatsForBlock(unsigned int dieNo, unsigned int blockNo)
	{
		unsigned int invalidSlices = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;
		unsigned int validSlices = USER_PAGES_PER_BLOCK - invalidSlices;
		unsigned int ageTicks = gcActivityTick - gcLastEraseTick[dieNo][blockNo];
		uint32_t score = CalculateCostBenefitScore(dieNo, blockNo);
		GC_DBG("[CB_GC][STAT] die=%d block=%d invalid=%u valid=%u age=%u score=%u\r\n",
				dieNo, blockNo, invalidSlices, validSlices, ageTicks, score);
	}

	// Validate: ensure selected victim has the maximum score in the list
	static void ValidateVictimSelection(unsigned int dieNo, unsigned int selectedBlock, uint32_t selectedScore)
	{
		int invalidSliceCnt;
		for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--)
		{
			unsigned int blockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;
			while (blockNo != BLOCK_NONE)
			{
				uint32_t score = CalculateCostBenefitScore(dieNo, blockNo);
				if (score > selectedScore)
				{
					GC_DBG("[CB_GC][ERROR] Found higher score than selected: die=%d candidate=%d score=%u selected=%d selscore=%u\r\n",
							dieNo, blockNo, score, selectedBlock, selectedScore);
					DumpGcStatsForBlock(dieNo, blockNo);
					DumpGcStatsForBlock(dieNo, selectedBlock);
					// Continue checking to report all offenders
				}
				blockNo = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
			}
		}
	}

	// Post-erase validation: ensure block state is clean after erase
	static void ValidatePostErase(unsigned int dieNo, unsigned int blockNo)
	{
		unsigned int pageNo;
		unsigned int vsa;
		unsigned int invalidCnt = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;
		unsigned int next = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
		unsigned int prev = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;

		if (invalidCnt != 0)
		{
			GC_DBG("[CB_GC][ERROR] Post-erase invalidSliceCnt != 0 die=%d block=%d invalid=%u\r\n", dieNo, blockNo, invalidCnt);
		}

		if ((next != BLOCK_NONE) || (prev != BLOCK_NONE))
		{
			GC_DBG("[CB_GC][ERROR] Post-erase next/prev not cleared die=%d block=%d next=%d prev=%d\r\n", dieNo, blockNo, next, prev);
		}

		for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
		{
			vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
			if (virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr != LSA_NONE)
			{
				GC_DBG("[CB_GC][ERROR] Post-erase virtualSlice not cleared die=%d block=%d page=%d vsa=%d logical=%d\r\n",
						dieNo, blockNo, pageNo, vsa, virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr);
			}
		}
	}

	/* Bump logical tick when a block becomes dirty */
	void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
	{
		if (invalidSliceCnt)    // age as logical counter
		{
			gcActivityTick++; // advance logical time
            GC_DBG("[CB_GC][DBG] tick++ die=%d block=%d invalid=%d tick=%d\r\n", dieNo, blockNo, invalidSliceCnt, gcActivityTick);
		}

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

        /* Event-driven automatic trigger: if free blocks drop below threshold,
         * select a victim and run GC immediately for this die.
         */
        if (virtualDieMapPtr->die[dieNo].freeBlockCnt <= CB_GC_TRIGGER_THRESHOLD)
        {
            unsigned int victim = GetFromGcVictimList(dieNo);
            if (victim != BLOCK_FAIL)
            {
                xil_printf("[CB_GC] Triggered by threshold (Die %d) freeBlockCnt=%d victim=%d\r\n",
                        dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt, victim);
                GarbageCollection(dieNo);
            }
            else
            {
                xil_printf("[CB_GC][WARN] No victim block available on die %d despite low freeBlockCnt=%d\r\n",
                        dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);
            }
        }
	}

	// Scan all candidates and pick the max (invalid*age)/(valid+1)
	// Accurate but costs proportional to candidate count.
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
			{
				unsigned int invalidSlices = virtualBlockMapPtr->block[dieNo][bestBlock].invalidSliceCnt;
				unsigned int validSlices = USER_PAGES_PER_BLOCK - invalidSlices;
				unsigned int ageTicks = gcActivityTick - gcLastEraseTick[dieNo][bestBlock];
				GC_DBG("[CB_GC][DBG] Selected victim die=%d block=%d score=%u invalid=%u valid=%u age=%u tick=%u\r\n",
						dieNo, bestBlock, bestScore, invalidSlices, validSlices, ageTicks, gcActivityTick);
			}

            /* Second-pass verification: re-scan candidates to protect against
             * any race or scoring inconsistency between the initial selection and
             * the moment of detachment. If a different block now has a higher
             * score, promote it to the selected victim and log the change.
             */
            {
                unsigned int recomputedBest = bestBlock;
                uint32_t recomputedScore = bestScore;
                int invalidSliceCnt2;
                for (invalidSliceCnt2 = SLICES_PER_BLOCK; invalidSliceCnt2 > 0; invalidSliceCnt2--)
                {
                    unsigned int blockNo2 = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt2].headBlock;
                    while (blockNo2 != BLOCK_NONE)
                    {
                        unsigned int nextBlock2 = virtualBlockMapPtr->block[dieNo][blockNo2].nextBlock;
                        uint32_t score2 = CalculateCostBenefitScore(dieNo, blockNo2);
                        if (score2 > recomputedScore)
                        {
                            recomputedScore = score2;
                            recomputedBest = blockNo2;
                        }
                        blockNo2 = nextBlock2;
                    }
                }

                if (recomputedBest != bestBlock)
                {
                    GC_DBG("[CB_GC][WARN] Victim changed on re-scan die=%d old=%d oldScore=%u new=%d newScore=%u\r\n",
                            dieNo, bestBlock, bestScore, recomputedBest, recomputedScore);
                    bestBlock = recomputedBest;
                    bestScore = recomputedScore;
                }

                // Validate the (final) selected victim (debug)
                ValidateVictimSelection(dieNo, bestBlock, bestScore);

                DetachBlockFromGcList(dieNo, bestBlock);
            }
        }
        else
		{
			xil_printf("[CB_GC][WARN] No victim block available on die %d\r\n", dieNo);
			assert(!"[WARNING] There are no free blocks. Abort terminate this ssd. [WARNING]");
		}

		return bestBlock;
	}


	/* Remove a single block from the GC candidate list */
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
#elif defined(CBGAME_GC)   // Cost-Benefit + Incremental GC (GameGC 방식)

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"
#include <stdint.h>

#define CB_GC_TRIGGER_THRESHOLD 1990
#define GC_PAGE_LIMIT 8     // incremental copy per cycle

#define GC_DBG(...) xil_printf(__VA_ARGS__)

P_GC_VICTIM_MAP gcVictimMapPtr;

unsigned int gcTriggered;
unsigned int copyCnt;
unsigned int globalVictimBlock[USER_DIES];

static unsigned int gcActivityTick;
static unsigned int gcLastEraseTick[USER_DIES][USER_BLOCKS_PER_DIE];
static unsigned int gcCount = 0;

/* incremental state-machine */
typedef struct {
    unsigned int state;
    unsigned int victimBlock;
    unsigned int curPage;
    unsigned char active;
} CBGAME_GC_CTX;

static CBGAME_GC_CTX cbCtx[USER_DIES];

enum {
    GC_STATE_IDLE = 0,
    GC_STATE_SELECT_VICTIM,
    GC_STATE_COPY_VALID_PAGES,
    GC_STATE_ERASE_BLOCK
};

static inline uint32_t CalculateCostBenefitScore(unsigned int dieNo, unsigned int blockNo);
static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo);
static void ValidatePostErase(unsigned int dieNo, unsigned int blockNo);

/* extern from FTL */
extern unsigned int Vorg2VsaTranslation(unsigned int dieNo, unsigned int blockNo, unsigned int pageNo);
extern void EraseBlock(unsigned int dieNo, unsigned int blockNo);
extern unsigned int GetFromFreeReqQ(void);
extern void SelectLowLevelReqQ(unsigned int reqSlotTag);
extern unsigned int AllocateTempDataBuf(unsigned int dieNo);
extern void UpdateTempDataBufEntryInfoBlockingReq(unsigned int entry, unsigned int reqSlotTag);
extern unsigned int FindFreeVirtualSliceForGc(unsigned int dieNo, unsigned int victimBlockNo);

/*===========================================================================*/
/* Init */
/*===========================================================================*/
void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;
    unsigned int blockNo;

    gcActivityTick = 0;
    gcVictimMapPtr = (P_GC_VICTIM_MAP)GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
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

        globalVictimBlock[dieNo] = BLOCK_NONE;
    }
}

/*===========================================================================*/
/* Victim selection*/
/*===========================================================================*/
unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int bestBlock = BLOCK_FAIL;
    uint32_t bestScore = 0;
    int invalidSliceCnt;

    for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--)
    {
        unsigned int blockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;
        while (blockNo != BLOCK_NONE)
        {
            unsigned int nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;

            uint32_t score = CalculateCostBenefitScore(dieNo, blockNo);
            if (score > bestScore)
            {
                bestScore = score;
                bestBlock = blockNo;
            }

            blockNo = nextBlock;
        }
    }

    if (bestBlock == BLOCK_FAIL)
    {
        xil_printf("[CBGAME_GC][WARN] No victim block available on die %d\r\n", dieNo);
        return BLOCK_FAIL;
    }

    {
        unsigned int invalidSlices = virtualBlockMapPtr->block[dieNo][bestBlock].invalidSliceCnt;
        unsigned int validSlices = USER_PAGES_PER_BLOCK - invalidSlices;
        unsigned int ageTicks = gcActivityTick - gcLastEraseTick[dieNo][bestBlock];

        GC_DBG("[CBGAME_GC][DBG] Selected victim die=%d block=%d score=%u invalid=%u valid=%u age=%u tick=%u\r\n",
               dieNo, bestBlock, bestScore, invalidSlices, validSlices, ageTicks, gcActivityTick);
    }

    DetachBlockFromGcList(dieNo, bestBlock);
    return bestBlock;
}

/*===========================================================================*/
/* PutToGcVictimList */
/*===========================================================================*/
void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (invalidSliceCnt)
    {
        gcActivityTick++;
        GC_DBG("[CBGAME_GC][DBG] tick++ die=%d block=%d invalid=%d tick=%d\r\n",
               dieNo, blockNo, invalidSliceCnt, gcActivityTick);
    }

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

/*===========================================================================*/
/* Incremental Scheduler */
/*===========================================================================*/
void GcScheduler(void)
{
    static unsigned int tick = 0;
    tick++;

    if (tick % 1000 == 0)
    {
        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            if (cbCtx[dieNo].active)
                GarbageCollection(dieNo);
            else if (virtualDieMapPtr->die[dieNo].freeBlockCnt <= CB_GC_TRIGGER_THRESHOLD)
                GarbageCollection(dieNo);
        }
    }

    if (tick % 100000 == 0)
    {
        for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
        {
            xil_printf("  Die %d: freeBlockCnt=%d\r\n",
                       dieNo, virtualDieMapPtr->die[dieNo].freeBlockCnt);
        }
    }
}

/*===========================================================================*/
/* Incremental GC*/
/*===========================================================================*/
void GarbageCollection(unsigned int dieNo)
{
    CBGAME_GC_CTX *ctx = &cbCtx[dieNo];
    unsigned int pageNo, vsa, lsa, req;
    unsigned int moved = 0;

    switch (ctx->state)
    {
    case GC_STATE_IDLE:
        ctx->state = GC_STATE_SELECT_VICTIM;
        ctx->active = 1;
        xil_printf("[CBGAME_GC] -> SELECT_VICTIM (Die %d)\r\n", dieNo);
        break;

    case GC_STATE_SELECT_VICTIM:
        ctx->victimBlock = GetFromGcVictimList(dieNo);

        if (ctx->victimBlock == BLOCK_FAIL)
        {
            xil_printf("[CBGAME_GC][WARN] No victim block available (Die %d)\r\n", dieNo);
            ctx->state = GC_STATE_IDLE;
            ctx->active = 0;
            return;
        }

        xil_printf("[CBGAME_GC] Die %d victim=%d\r\n", dieNo, ctx->victimBlock);
        xil_printf("[CBGAME_GC] GC start die=%d block=%d\r\n", dieNo, ctx->victimBlock);

        ctx->curPage = 0;
        ctx->state = GC_STATE_COPY_VALID_PAGES;
        break;

    case GC_STATE_COPY_VALID_PAGES:

        moved = 0;

        while (moved < GC_PAGE_LIMIT && ctx->curPage < USER_PAGES_PER_BLOCK)
        {
            pageNo = ctx->curPage++;
            vsa = Vorg2VsaTranslation(dieNo, ctx->victimBlock, pageNo);
            lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;

            if (lsa != LSA_NONE &&
                logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr == vsa)
            {
                unsigned int newVsa = FindFreeVirtualSliceForGc(dieNo, ctx->victimBlock);
                unsigned int tmp = AllocateTempDataBuf(dieNo);

                req = GetFromFreeReqQ();
                reqPoolPtr->reqPool[req].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[req].reqCode = REQ_CODE_READ;
                reqPoolPtr->reqPool[req].nandInfo.virtualSliceAddr = vsa;
                reqPoolPtr->reqPool[req].dataBufInfo.entry = tmp;
                SelectLowLevelReqQ(req);

                req = GetFromFreeReqQ();
                reqPoolPtr->reqPool[req].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[req].reqCode = REQ_CODE_WRITE;
                reqPoolPtr->reqPool[req].nandInfo.virtualSliceAddr = newVsa;
                reqPoolPtr->reqPool[req].dataBufInfo.entry = tmp;
                SelectLowLevelReqQ(req);

                logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr = newVsa;
                virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr = lsa;

                moved++;
            }
        }

        if (ctx->curPage >= USER_PAGES_PER_BLOCK)
            ctx->state = GC_STATE_ERASE_BLOCK;

        break;

    case GC_STATE_ERASE_BLOCK:

        EraseBlock(dieNo, ctx->victimBlock);
        xil_printf("[CBGAME_GC] Erased die=%d block=%d\r\n", dieNo, ctx->victimBlock);

        ValidatePostErase(dieNo, ctx->victimBlock);

        gcLastEraseTick[dieNo][ctx->victimBlock] = gcActivityTick;
        gcCount++;

        xil_printf("[CBGAME_GC] GC completed for die=%d block=%d\r\n",
                   dieNo, ctx->victimBlock);

        ctx->victimBlock = BLOCK_NONE;
        ctx->state = GC_STATE_IDLE;
        ctx->active = 0;

        break;
    }
}

/*===========================================================================*/
/* Validation */
/*===========================================================================*/
static void ValidatePostErase(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int pageNo, vsa;

    if (virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt != 0)
    {
        GC_DBG("[CBGAME_GC][ERROR] Post-erase invalidSliceCnt != 0 die=%d block=%d\r\n",
               dieNo, blockNo);
    }

    for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
    {
        vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);

        if (virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr != LSA_NONE)
        {
            GC_DBG("[CBGAME_GC][ERROR] Post-erase VSA not cleared die=%d block=%d page=%d\r\n",
                   dieNo, blockNo, pageNo);
        }
    }
}

/*===========================================================================*/
/* DetachBlockFromGcList */
/*===========================================================================*/
static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo)
{
    SelectiveGetFromGcVictimList(dieNo, blockNo);
    virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
    virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;

    GC_DBG("[CBGAME_GC][DBG] Detached block die=%d block=%d\r\n",
           dieNo, blockNo);
}

/*===========================================================================*/
/* Selective removal */
/*===========================================================================*/
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
