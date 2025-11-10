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
// NOTE (2025-11): This file implements the CAT (Cost-Age Tradeoff) garbage
// collection policy for selecting GC victims.
//
//   Public function names (e.g., GetFromGcVictimList) and data structures are
//   intentionally preserved for compatibility with the rest of the firmware.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include "xil_printf.h"
#include <assert.h>
#include <stdint.h>
#include "memory_map.h"

P_GC_VICTIM_MAP gcVictimMapPtr;

// [CAT] Lightweight logical time for block invalidation “age”
static unsigned int gcActivityTick;
// [CAT] Last tick when a block’s invalid count became non-zero (per die/block)
static unsigned int gcLastInvalidTick[USER_DIES][USER_BLOCKS_PER_DIE];

// Forward declarations (keep existing external interfaces)
static inline uint32_t CalculateCatScore(unsigned int dieNo, unsigned int blockNo);
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
    unsigned int blockNo;   // [CAT-ADD] 블록별 나이 초기화용 인덱스

    gcActivityTick = 0;     // [CAT-ADD] 논리 시계 초기화 (age 계산 기준)

    gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

    for (dieNo = 0; dieNo < USER_DIES; dieNo++)
    {
        // [COMMON] 버킷(무효 슬라이스 수)별 후보 리스트 초기화 (greedy, cat 공통)
        for (invalidSliceCnt = 0; invalidSliceCnt < SLICES_PER_BLOCK + 1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }

        // [CAT-ADD] 블록별 '마지막 invalid 발생 tick' 초기화, age 계산에 사용
        for (blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo++)
            gcLastInvalidTick[dieNo][blockNo] = 0;
    }
}

// ----------------------------- Main GC routine -------------------------------
void GarbageCollection(unsigned int dieNo)
{
    unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;

    // [CAT-CHG][정책 캡슐화] 희생 블록 선택 정책은 GetFromGcVictimList 내부로 숨김.
    //  - Greedy: "가장 큰 invalid 버킷의 head pop"을 내부에서 수행
    //  - CAT(reverse): (benefit/cost) 점수화로 전수 스캔 후 best 선택 + 중간 노드 분리
    //  - 외부 시그니처/이름 유지 → 상위 로직 영향 최소화
    victimBlockNo = GetFromGcVictimList(dieNo);
    dieNoForGcCopy = dieNo;

    // [COMMON] 선택된 victim 블록이 모두 invalid가 아니면(valid가 있으면) 유효 데이터 이주 수행
    if (virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
    {
        for (pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            // [COMMON] valid 여부 확인 (논리→가상 양방향 매핑의 일치성)
            if (logicalSliceAddr != LSA_NONE)
                if (logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr == virtualSliceAddr) // valid data
                {
                    // ---------------------------- READ ----------------------------
                    // [COMMON] 읽기 요청 구성 및 디스패치(진짜 하드웨어에서 수행되도록 전달하는 것) (변화 없음)
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
                    // [COMMON] 쓰기 요청 구성 및 디스패치(진짜 하드웨어에서 수행되도록 전달하는 것) (변화 없음)
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
                    
                    // [COMMON] GC 대상 다이에서 새 가상 슬라이스 할당 (변화 없음)
                    reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = FindFreeVirtualSliceForGc(dieNoForGcCopy, victimBlockNo);

                    // [COMMON] 매핑 갱신 (논리→가상 / 가상→논리) (변화 없음)
                    logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
                    virtualSliceMapPtr->virtualSlice[reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr].logicalSliceAddr = logicalSliceAddr;
                    SelectLowLevelReqQ(reqSlotTag);
                }
        }
    }

    EraseBlock(dieNo, victimBlockNo);

    // [CAT] Victim block was reset by erase; record current tick as last invalid tick baseline.
    gcLastInvalidTick[dieNo][victimBlockNo] = gcActivityTick;
}

// --------------------------- GC list manipulation ----------------------------
// [Greedy] 임의 블록을 GC 후보 리스트에서 안전하게 제거하는 헬퍼
// - head/tail 및 prev/next 정리
// - 중간 노드 제거 시 사용 (O(1) 분리)
static inline void DetachBlockFromGcList(unsigned int dieNo, unsigned int blockNo)
{
    SelectiveGetFromGcVictimList(dieNo, blockNo);
    virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
    virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
}


// NOTE: Called whenever a block moves between invalid-count bins.
// We bump the activity tick iff invalidSliceCnt > 0 so “age since became dirty”
// is meaningful. This is a very cheap logical timestamp (no timers needed).
// [CAT(reverse)] 리스트 삽입은 동일하되, dirty된 시점의 논리시간을 업데이트(age 추적)
void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if (invalidSliceCnt)    // dirty 상태일 때만 시간 올라가도록
    {
        gcActivityTick++; // [CAT] age 기준이 되는 tick 증가
        gcLastInvalidTick[dieNo][blockNo] = gcActivityTick; // 블록별 마지막 dirty 시각을 기록
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

// [CAT(reverse)] 후보 전수 스캔 + 점수 최대화 선택 (O(N) 선택)
// score ≈ (invalid+1)*(age+1) / ((valid+1)*(wear+1))
// - invalid↑, age↑  → 이득↑ (청소 우선)   // 기아 방지·장기공정성
// - valid↑, wear↑   → 비용↑ (연기)        // 이주비용·웨어레벨링 반영
// 선택된 블록이 리스트 중간이면 Detach로 안전 제거
unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    unsigned int bestBlock = BLOCK_FAIL;
    uint32_t bestScore = 0;
    int invalidSliceCnt;

    // [선정] 모든 버킷을 훑으며 최고 점수 블록 탐색
    for (invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0; invalidSliceCnt--) {
        unsigned int blockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;
        while (blockNo != BLOCK_NONE) {
            // 순회 중 구조 변화 대비: next를 먼저 백업
            unsigned int nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;

            uint32_t score = CalculateCatScore(dieNo, blockNo);
            if (score > bestScore) {
                bestScore = score;
                bestBlock = blockNo;
            }
            blockNo = nextBlock;
        }
    }

    // [제거] 선택된 블록을 리스트에서 안전 분리 (head/중간/tail 모두 처리)
    if (bestBlock != BLOCK_FAIL) {
        DetachBlockFromGcList(dieNo, bestBlock);
    } else {
        assert(!"[WARNING] There are no free blocks. Abort terminate this ssd. [WARNING]");
    }

    return bestBlock;
}


// --------------------------- CAT Scoring ----------------------------
// Keep computations lightweight for firmware:
//  - integers only (use 64-bit for safe intermediate multiply)
//  - add +1 guards to avoid zero-division & favor decisive differences
static inline uint32_t CalculateCatScore(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int invalidSlices = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;
    unsigned int validSlices   = USER_PAGES_PER_BLOCK - invalidSlices;
    unsigned int ageTicks      = gcActivityTick - gcLastInvalidTick[dieNo][blockNo];
    unsigned int wearCount     = virtualBlockMapPtr->block[dieNo][blockNo].eraseCnt;

    uint64_t numerator   = (uint64_t)(invalidSlices + 1) * (uint64_t)(ageTicks + 1);
    uint64_t denominator = (uint64_t)(validSlices   + 1) * (uint64_t)(wearCount + 1);

    if (denominator == 0) // ultra-defensive; practically never hit
        return (uint32_t)numerator;

    return (uint32_t)(numerator / denominator);
}

//GC 후보 리스트에서 특정 블록만 빼내고 싶을 때 사용
// -------------------------- Greedy와 동일 --------------------------
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
