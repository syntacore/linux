/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 *  Copyright (C) 2026 Syntacore
 */

#ifndef _DTS_SCR_PMA_H
#define _DTS_SCR_PMA_H

#define SCR_PMA_NC                  (1U << 0)                                   /* PMA region non-cacheable */
#define SCR_PMA_SO                  (1U << 1)                                   /* PMA region strongly-ordered */
#define SCR_PMA_NI                  (1U << 2)                                   /* PMA region non-idempotent */
#define SCR_PMA_AMO                 (1U << 3)                                   /* PMA region AMO */
#define SCR_PMA_LR_SC               (1U << 4)                                   /* PMA region reservability */

/* PMA pmaxcfg legal values */
#define SCR_PMA_MEM_REGION_C_WO     (SCR_PMA_AMO | SCR_PMA_LR_SC)               /* Cacheable idempotent main memory */
#define SCR_PMA_MEM_REGION_NC_SO    (SCR_PMA_NC | SCR_PMA_SO)                   /* Non-cacheable idempotent IO */
#define SCR_PMA_MEM_REGION_NC_WO    (SCR_PMA_NC)                                /* Non-cacheable idempotent main memory */
#define SCR_PMA_MEM_REGION_IO       (SCR_PMA_NC | SCR_PMA_SO | SCR_PMA_NI)      /* Non-cacheable non-idempotent IO */

#endif /* _DTS_SCR_PMA_H */
