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
#define GAME_GC // wdy: GAME GC 활성화
// #define CB_GC // wdy: Cost_Benefit GC 활성화

#include "garbage_collection.h"

#if defined(ORIGINAL_GC)

	#include "xil_printf.h"
	#include <assert.h>
	#include "memory_map.h"

	P_GC_VICTIM_MAP gcVictimMapPtr;

	void InitGcVictimMap()
	{
		int dieNo, invalidSliceCnt;

		gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

		for(dieNo=0 ; dieNo<USER_DIES; dieNo++)
		{
			for(invalidSliceCnt=0 ; invalidSliceCnt<SLICES_PER_BLOCK+1; invalidSliceCnt++)
			{
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
			}
		}
	}


	void GarbageCollection(unsigned int dieNo)
	{
		unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;

		victimBlockNo = GetFromGcVictimList(dieNo);
		dieNoForGcCopy = dieNo;

		if(virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
		{
			for(pageNo=0 ; pageNo<USER_PAGES_PER_BLOCK ; pageNo++)
			{
				virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
				logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

				if(logicalSliceAddr != LSA_NONE)
					if(logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr ==  virtualSliceAddr) //valid data
					{
						//read
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

						//write
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
	}


	void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
	{
		if(gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
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

	unsigned int GetFromGcVictimList(unsigned int dieNo)
	{
		unsigned int evictedBlockNo;
		int invalidSliceCnt;

		for(invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0 ; invalidSliceCnt--)
		{
			if(gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock != BLOCK_NONE)
			{
				evictedBlockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;

				if(virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock != BLOCK_NONE)
				{
					virtualBlockMapPtr->block[dieNo][virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock].prevBlock = BLOCK_NONE;
					gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock;

				}
				else
				{
					gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
					gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
				}
				return evictedBlockNo;

			}
		}

		assert(!"[WARNING] There are no free blocks. Abort terminate this ssd. [WARNING]");
		return BLOCK_FAIL;
	}


	void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
	{
		unsigned int nextBlock, prevBlock, invalidSliceCnt;

		nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
		prevBlock = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;
		invalidSliceCnt = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;

		if((nextBlock != BLOCK_NONE) && (prevBlock != BLOCK_NONE))
		{
			virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = nextBlock;
			virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = prevBlock;
		}
		else if((nextBlock == BLOCK_NONE) && (prevBlock != BLOCK_NONE))
		{
			virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = BLOCK_NONE;
			gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = prevBlock;
		}
		else if((nextBlock != BLOCK_NONE) && (prevBlock == BLOCK_NONE))
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

	//////////////////////////////////////////////////////////////////////////////////
	// garbage_collection.c for Cosmos+ OpenSSD
	// Incremental + Mark & Sweep + Generational GC (updated)
	// Key differences from legacy GC:
	//   - State-based incremental GC (non-blocking per-die)
	//   - Mark & Sweep victim selection
	//   - Generational scoring (young vs old data)
	//////////////////////////////////////////////////////////////////////////////////

	#include "xil_printf.h"
	#include <assert.h>
	#include "memory_map.h"

	#define GC_SCHED_INTERVAL_TICK 10        // GC scheduling frequency
	#define GC_PAGE_LIMIT 8                  // Copy only limited pages per step
	#define GC_TRIGGER_FREE_BLOCK_THRESHOLD 4 // Trigger GC when free blocks are low

	// === BitMap Utilities: Used in Mark&Sweep Algorithm ===
	#ifndef MARKSWEEP_BIT
	#define MARKSWEEP_BIT(arr, i)   ( (arr)[(i)>>3] &  (1u << ((i)&7)) )
	#define MARKSWEEP_SET(arr, i)   ( (arr)[(i)>>3] |= (1u << ((i)&7)) )
	#define MARKSWEEP_CLR(arr, i)   ( (arr)[(i)>>3] &= ~(1u << ((i)&7)) )
	#endif

	// === Generational Options ===
	#define GEN_TICKS 1000          // epoch flip interval
	#define GEN_WEIGHT_OLD 8        // old data cost weight
	#define GEN_WEIGHT_YOUNG 1      // young data cost weight

	unsigned int gcGenerationalParity = 0; // current generation parity

	// === Mark & Generation bitmaps ===
	static unsigned char s_gcLiveMark[(SLICES_PER_SSD + 7) / 8];
	static unsigned char s_gcGenBit[(SLICES_PER_SSD + 7) / 8];

	P_GC_VICTIM_MAP gcVictimMapPtr;
	unsigned int gcTriggered;
	unsigned int copyCnt;
	INCREMENTAL_GC_CONTEXT gcCtx[USER_DIES]; // per-die incremental GC context

	//-------------------------------------------------------------------------------
	// Check if GC should be triggered
	//-------------------------------------------------------------------------------
	int NeedGc(unsigned int dieNo)
	{
		return (virtualDieMapPtr->die[dieNo].freeBlockCnt <= GC_TRIGGER_FREE_BLOCK_THRESHOLD);
	}

	//-------------------------------------------------------------------------------
	// FlipGeneration: periodically flips generation parity
	//-------------------------------------------------------------------------------
	void FlipGeneration(void)
	{
		gcGenerationalParity ^= 1;
		xil_printf("[GEN] Flip generation -> parity=%u\r\n", gcGenerationalParity);
	}

	//-------------------------------------------------------------------------------
	// LsaWriteNote: mark LSA as recently updated
	//-------------------------------------------------------------------------------
	void LsaWriteNote(unsigned int logicalSliceAddr)
	{
		if (logicalSliceAddr >= SLICES_PER_SSD) return;

		if (gcGenerationalParity)
			MARKSWEEP_SET(s_gcGenBit, logicalSliceAddr);
		else
			MARKSWEEP_CLR(s_gcGenBit, logicalSliceAddr);
	}

	//-------------------------------------------------------------------------------
	// Incremental GC scheduler
	//-------------------------------------------------------------------------------
	void GcScheduler()
	{
		static unsigned int tick = 0;
		static unsigned int genTick = 0;
		tick++;
		genTick++;

		// flip generation every GEN_TICKS
		if (genTick % GEN_TICKS == 0)
			FlipGeneration();

		if (tick % GC_SCHED_INTERVAL_TICK == 0)
		{
			for (int dieNo = 0; dieNo < USER_DIES; dieNo++)
			{
				if (gcCtx[dieNo].active)
					GarbageCollection(dieNo);
				else if (NeedGc(dieNo))
					GarbageCollection(dieNo);
			}
		}
	}

	//-------------------------------------------------------------------------------
	// Manually trigger incremental GC (address_translation.c)
	//-------------------------------------------------------------------------------
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

	//-------------------------------------------------------------------------------
	// BuildGcLiveMark: Mark phase - identify all live LSA
	//-------------------------------------------------------------------------------
	void BuildGcLiveMark(void)
	{
		for (unsigned int i = 0; i < sizeof(s_gcLiveMark); i++)
			s_gcLiveMark[i] = 0;

		for (unsigned int lsa = 0; lsa < SLICES_PER_SSD; lsa++)
		{
			unsigned int vsa = logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr;
			if (vsa != VSA_NONE)
				MARKSWEEP_SET(s_gcLiveMark, lsa);
		}
	}

	//-------------------------------------------------------------------------------
	// Count valid pages + generational info
	//-------------------------------------------------------------------------------
	static inline void CountValidPages_Generational(
		unsigned int dieNo, unsigned int blockNo,
		unsigned int *outYoung, unsigned int *outOld)
	{
		unsigned int young = 0, old = 0;

		if (virtualBlockMapPtr->block[dieNo][blockNo].bad)
		{
			*outYoung = 0; *outOld = USER_PAGES_PER_BLOCK;
			return;
		}

		for (unsigned int page = 0; page < USER_PAGES_PER_BLOCK; page++)
		{
			unsigned int vsa = Vorg2VsaTranslation(dieNo, blockNo, page);
			unsigned int lsa = virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr;

			if (lsa != LSA_NONE && MARKSWEEP_BIT(s_gcLiveMark, lsa))
			{
				if (logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr == vsa)
				{
					unsigned int genBit = !!MARKSWEEP_BIT(s_gcGenBit, lsa);
					if (genBit == gcGenerationalParity)
						young++;
					else
						old++;
				}
			}
		}
		*outYoung = young;
		*outOld = old;
	}

	//-------------------------------------------------------------------------------
	// Initialize victim map and incremental GC contexts
	//-------------------------------------------------------------------------------
	void InitGcVictimMap()
	{
		int dieNo, invalidSliceCnt;

		gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

		for (unsigned int i = 0; i < sizeof(s_gcGenBit); i++)
			s_gcGenBit[i] = 0;
		gcGenerationalParity = 0;

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

	//-------------------------------------------------------------------------------
	// Incremental GC state machine
	//-------------------------------------------------------------------------------
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

					// read old page
					reqSlotTag = GetFromFreeReqQ();
					reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
					reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;
					reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = tempBuf;
					SelectLowLevelReqQ(reqSlotTag);

					// write new page
					reqSlotTag = GetFromFreeReqQ();
					reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
					reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = newVsa;
					reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = tempBuf;
					SelectLowLevelReqQ(reqSlotTag);

					// update mapping
					logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = newVsa;
					virtualSliceMapPtr->virtualSlice[newVsa].logicalSliceAddr = logicalSliceAddr;

					// note generation update
					LsaWriteNote(logicalSliceAddr);
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

	//-------------------------------------------------------------------------------
	// PutToGcVictimList (unchanged)
	//-------------------------------------------------------------------------------
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

	//-------------------------------------------------------------------------------
	// GetFromGcVictimList (Mark & Sweep + Generational scoring)
	//-------------------------------------------------------------------------------
	unsigned int GetFromGcVictimList(unsigned int dieNo)
	{
		unsigned int bestBlock = BLOCK_NONE;
		unsigned int bestScore = 0xFFFFFFFF;
		unsigned int bestErase = 0;

		BuildGcLiveMark();

		for (unsigned int block = 0; block < USER_BLOCKS_PER_DIE; block++)
		{
			if (virtualBlockMapPtr->block[dieNo][block].free) continue;
			if (virtualBlockMapPtr->block[dieNo][block].bad) continue;

			unsigned int youngCnt, oldCnt;
			CountValidPages_Generational(dieNo, block, &youngCnt, &oldCnt);

			unsigned int score = oldCnt * GEN_WEIGHT_OLD + youngCnt * GEN_WEIGHT_YOUNG;

			if (score < bestScore ||
				(score == bestScore && virtualBlockMapPtr->block[dieNo][block].eraseCnt > bestErase))
			{
				bestScore = score;
				bestErase = virtualBlockMapPtr->block[dieNo][block].eraseCnt;
				bestBlock = block;
				if (bestScore == 0) break;
			}
		}

		if (bestBlock != BLOCK_NONE)
		{
			SelectiveGetFromGcVictimList(dieNo, bestBlock);
			xil_printf("[MS+GEN] Die %u victim=%u score=%u\r\n", dieNo, bestBlock, bestScore);
		}
		else
			xil_printf("[MS+GEN] No victim block found (Die %u)\r\n", dieNo);

		return bestBlock;
	}

	//-------------------------------------------------------------------------------
	// SelectiveGetFromGcVictimList (unchanged)
	//-------------------------------------------------------------------------------
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

	#if 1
	// 런타임 디버그 출력 토글
	static int gcDebugRuntime = 1;
	static inline void SetGcDebugRuntime(int on) { gcDebugRuntime = on; }
	#define GC_DBG(...) do { if (gcDebugRuntime) xil_printf(__VA_ARGS__); } while(0)
	#endif

	P_GC_VICTIM_MAP gcVictimMapPtr;

	// 외부 인터페이스와의 호환성을 위한 변수들
	unsigned int gcTriggered;
	unsigned int copyCnt;

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
		}
	}
		// ----------------------------- 메인 GC 루틴 -------------------------------
	void GarbageCollection(unsigned int dieNo)
	{
		unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;
		unsigned int movedPages = 0;
		unsigned int movedLogical[USER_PAGES_PER_BLOCK];
		unsigned int movedDestVsa[USER_PAGES_PER_BLOCK];

		victimBlockNo = GetFromGcVictimList(dieNo);
		GC_DBG("[GC] Start GC: die=%d victim=%d tick=%d\n", dieNo, victimBlockNo, gcActivityTick);
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
					GC_DBG("[GC-ERROR] Mapping mismatch after GC die=%d victim=%d lsa=%d expected_vsa=%d actual_vsa=%d\n",
							dieNo, victimBlockNo, lsa, vsa, logicalSliceMapPtr->logicalSlice[lsa].virtualSliceAddr);
				}
			}
		}

		GC_DBG("[GC] Moved %d pages during GC die=%d victim=%d\n", movedPages, dieNo, victimBlockNo);

		EraseBlock(dieNo, victimBlockNo);

		/* 즉시 재선택 방지를 위해 erase 시각 기록 */
		gcLastEraseTick[dieNo][victimBlockNo] = gcActivityTick;
		gcCount++;
		GC_DBG("[GC] Erased die=%d block=%d total_gc_count=%d\n", dieNo, victimBlockNo, gcCount);
		/* erase 후 상태 검증 */
		ValidatePostErase(dieNo, victimBlockNo);
	}
	// --------------------------- GC 리스트 조작 ----------------------------
	// Detach helper: remove block from list and clear next/prev links
	static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo)
	{
		SelectiveGetFromGcVictimList(dieNo, blockNo);
		virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
		virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
		GC_DBG("[GC] Detached block die=%d block=%d\n", dieNo, blockNo);
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
		GC_DBG("[GC-STAT] die=%d block=%d invalid=%u valid=%u age=%u score=%u\n",
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
					GC_DBG("[GC-ERROR] Found higher score than selected: die=%d candidate=%d score=%u selected=%d selscore=%u\n",
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
			GC_DBG("[GC-ERROR] Post-erase invalidSliceCnt != 0 die=%d block=%d invalid=%u\n", dieNo, blockNo, invalidCnt);
		}

		if ((next != BLOCK_NONE) || (prev != BLOCK_NONE))
		{
			GC_DBG("[GC-ERROR] Post-erase next/prev not cleared die=%d block=%d next=%d prev=%d\n", dieNo, blockNo, next, prev);
		}

		for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
		{
			vsa = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
			if (virtualSliceMapPtr->virtualSlice[vsa].logicalSliceAddr != LSA_NONE)
			{
				GC_DBG("[GC-ERROR] Post-erase virtualSlice not cleared die=%d block=%d page=%d vsa=%d logical=%d\n",
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
			GC_DBG("[GC] tick++ die=%d block=%d invalid=%d tick=%d\n", dieNo, blockNo, invalidSliceCnt, gcActivityTick);
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
				GC_DBG("[GC] Selected victim die=%d block=%d score=%u invalid=%u valid=%u age=%u tick=%u\n",
						dieNo, bestBlock, bestScore, invalidSlices, validSlices, ageTicks, gcActivityTick);
			}

			// Validate the selected victim (debug)
			ValidateVictimSelection(dieNo, bestBlock, bestScore);

			DetachBlockFromGcList(dieNo, bestBlock);
		}
		else
		{
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


#endif
