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

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"

P_GC_VICTIM_MAP gcVictimMapPtr;

// ----------------------------- Initialization --------------------------------
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

// ----------------------------- Main GC routine -------------------------------
void GarbageCollection(unsigned int dieNo)
{
	unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;

	victimBlockNo = GetFromGcVictimList(dieNo);
	dieNoForGcCopy = dieNo;

	// [COMMON] 선택된 victim 블록이 모두 invalid가 아니면(valid가 있으면) 유효 데이터 이주 수행
	if(virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
	{
		for(pageNo=0 ; pageNo<USER_PAGES_PER_BLOCK ; pageNo++)
		{
			virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
			logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

			// [COMMON] valid 여부 확인 (논리→가상 양방향 매핑의 일치성)
			if(logicalSliceAddr != LSA_NONE)
				if(logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr ==  virtualSliceAddr) //valid data
				{
					// ---------------------------- READ ----------------------------
					// [COMMON] 읽기 요청 구성 및 디스패치(진짜 하드웨어에서 수행되도록 전달하는 것)
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
					// [COMMON] 쓰기 요청 구성 및 디스패치 (진짜 하드웨어에서 수행되도록 전달하는 것)
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
					
					// [COMMON] GC 대상 다이에서 새 가상 슬라이스 할당
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = FindFreeVirtualSliceForGc(dieNoForGcCopy, victimBlockNo);

					// [COMMON] 매핑 갱신 (논리→가상 / 가상→논리)
					logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
					virtualSliceMapPtr->virtualSlice[reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr].logicalSliceAddr = logicalSliceAddr;

					SelectLowLevelReqQ(reqSlotTag);
				}
		}
	}

	EraseBlock(dieNo, victimBlockNo);
}

// 버킷(무효 슬라이스 개수 invalidSliceCnt)의 양단 연결 리스트에 그냥 넣기만 함.
// 시간/나이 개념(얼마나 오래 방치되었는가)은 고려 X.
// 나중에 희생 블록 선택은 “무효가 가장 많은 버킷에서 먼저 온 놈” 같은 단순 규칙으로 처리.
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

// GetFromGcVictimList가 한 함수 안에서
// “무효 슬라이스 수가 가장 큰 버킷부터” 훑고,
// 가장 무효가 많은 버킷부터 head를 즉시 pop (O(1) 선택)(연결 리스트 포인터 갱신)해서 곧바로 리턴까지
// 즉, 선정 + 분리까지 전부 수행
// - 항상 head 제거이므로 포인터 갱신이 단순
unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int evictedBlockNo;
    int invalidSliceCnt;

    for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--) {
        if (gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock != BLOCK_NONE) {
            evictedBlockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;

            // head pop: 다음 노드를 새 head로, 없으면 head/tail 모두 NONE
            if (virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock != BLOCK_NONE) {
                virtualBlockMapPtr->block[dieNo]
                    [virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock].prevBlock = BLOCK_NONE;
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock =
                    virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock;
            } else {
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
                gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
            }
            return evictedBlockNo;
        }
    }

    assert(!"[WARNING] There are no free blocks. Abort terminate this ssd. [WARNING]");
    return BLOCK_FAIL;
}

//GC 후보 리스트에서 특정 블록만 빼내고 싶을 때 사용
void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int nextBlock, prevBlock, invalidSliceCnt;

    nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;       // 현재 블록의 다음 노드
    prevBlock = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;       // 현재 블록의 이전 노드
    invalidSliceCnt = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt; // 블록이 속한 버킷 인덱스 (무효 슬라이스 수)

    if ((nextBlock != BLOCK_NONE) && (prevBlock != BLOCK_NONE))            // ① 중간 노드일 경우 (prev도 있고 next도 있음)
    {
        virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = nextBlock; // prev → next 연결
        virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = prevBlock; // next → prev 연결
    }
    else if ((nextBlock == BLOCK_NONE) && (prevBlock != BLOCK_NONE))       // ② tail 노드 (뒤 노드 없음, 앞 노드만 있음)
    {
        virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = BLOCK_NONE; // prev가 새 tail 이므로 nextBlock = NONE
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = prevBlock; // tail 갱신
    }
    else if ((nextBlock != BLOCK_NONE) && (prevBlock == BLOCK_NONE))       // ③ head 노드 (앞 노드 없음, 뒤 노드만 있음)
    {
        virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = BLOCK_NONE; // next가 새 head, prevBlock = NONE
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = nextBlock; // head 갱신
    }
    else                                                                   // ④ 단독 노드 (prev, next 둘 다 없음 ⇒ 리스트 크기=1)
    {
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE; // head 제거
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE; // tail 제거
    }
}

