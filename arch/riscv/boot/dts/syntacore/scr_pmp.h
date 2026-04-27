/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 *  Copyright (C) 2026 Syntacore
 */

#ifndef _DTS_SCR_PMP_H
#define _DTS_SCR_PMP_H

#define SCR_PMP_MMODE_READ      (1UL << 0)
#define SCR_PMP_MMODE_WRITE     (1UL << 1)
#define SCR_PMP_MMODE_EXECUTE   (1UL << 2)
#define SCR_PMP_SUMODE_READ     (1UL << 3)
#define SCR_PMP_SUMODE_WRITE    (1UL << 4)
#define SCR_PMP_SUMODE_EXECUTE  (1UL << 5)

#define SCR_MEMREGION_MMIO      (1UL << 31)

#define SCR_PMA_FLAG        (1 << 29)

#define SCR_PMA_OFF         16
#define SCR_PMA_MASK        3
#define SCR_PMA_PMP_OFF     5

#define SCR_PMA(a)          ((((a) & SCR_PMA_MASK) << SCR_PMA_OFF) | SCR_PMA_FLAG)
#define SCR_PMA_UNC         SCR_PMA(1)
#define SCR_PMA_MMIO        SCR_PMA(3)
#define SCR_PMA_BITMASK     (SCR_PMA_FLAG | (SCR_PMA_MASK << SCR_PMA_OFF))

#endif /* _DTS_SCR_PMP_H */
