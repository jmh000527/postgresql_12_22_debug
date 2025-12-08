/*-------------------------------------------------------------------------
 *
 * sdir.h
 *	  POSTGRES scan direction definitions.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sdir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SDIR_H
#define SDIR_H


/*
 * ScanDirection 原本是 int8 类型，但原因不明。保留原始值以避免潜在问题。-ay 2/95
 * 扫描方向枚举类型定义：
 *   BackwardScanDirection      向后扫描
 *   NoMovementScanDirection    不移动
 *   ForwardScanDirection       向前扫描
 */
typedef enum ScanDirection
{
	BackwardScanDirection = -1,    // 向后扫描
	NoMovementScanDirection = 0,   // 不移动
	ForwardScanDirection = 1       // 向前扫描
} ScanDirection;

/*
 * ScanDirectionIsValid
 *		如果扫描方向是有效的，则返回 true。
 */
#define ScanDirectionIsValid(direction) \
	((bool) (BackwardScanDirection <= (direction) && \
			 (direction) <= ForwardScanDirection))

/*
 * ScanDirectionIsBackward
 *      如果扫描方向是 BackwardScanDirection，则返回 true。
 */
#define ScanDirectionIsBackward(direction) \
	((bool) ((direction) == BackwardScanDirection))

/*
 * ScanDirectionIsNoMovement
 *		如果扫描方向是 NoMovementScanDirection，则返回 true。
 */
#define ScanDirectionIsNoMovement(direction) \
	((bool) ((direction) == NoMovementScanDirection))

/*
 * ScanDirectionIsForward
 *		如果扫描方向是 ForwardScanDirection，则返回 true。
 */
#define ScanDirectionIsForward(direction) \
	((bool) ((direction) == ForwardScanDirection))

#endif							/* SDIR_H */
