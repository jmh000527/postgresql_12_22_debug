/*-------------------------------------------------------------------------
 *
 * outline_guc.c
 *	  GUC parameters for outline system
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/outline/outline_guc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/outline_hints.h"
#include "utils/guc.h"

/* GUC parameter */
bool outline_display_hints = false;

/*
 * Module initialization
 */
void
outline_init_guc(void)
{
	DefineCustomBoolVariable("outline.display_hints",
							 "Display generated outline data after each query execution.",
							 "When enabled, automatically generates and displays hints "
							 "from execution plans in OceanBase/Oracle style format.",
							 &outline_display_hints,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
}
