// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implementation of get_cpuid().
 *
 * Author: Nikita Shubin <n.shubin@yadro.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <api/fs/fs.h>
#include <errno.h>
#include "../../util/debug.h"
#include "../../util/header.h"

#include <asm/unistd.h>
#include <asm/hwprobe.h>
#include <internal/cpumap.h>

enum {
	MVENDORID = 0,
	MARCHID,
	MIMPID,
};

static char *_get_cpuid(struct perf_cpu_map *cpus)
{
	char *cpuid = NULL;
	unsigned long cpu_mask = 0UL;
	unsigned long cpu = RC_CHK_ACCESS(cpus)->map[0].cpu;

	struct riscv_hwprobe query[] = {[MVENDORID] = {RISCV_HWPROBE_KEY_MVENDORID, 0},
					[MARCHID]   = {RISCV_HWPROBE_KEY_MARCHID,   0},
					[MIMPID]    = {RISCV_HWPROBE_KEY_MIMPID,    0}};

	if (cpu >= sizeof(cpu_mask) * 8)
		goto exit;

	cpu_mask |= (1UL << cpu);

	if (syscall(__NR_riscv_hwprobe, &query[0], ARRAY_SIZE(query), 1, &cpu_mask, 0))
		goto exit;

	if (asprintf(&cpuid, "0x%llx-0x%llx-0x%llx", query[MVENDORID].value,
			query[MARCHID].value, query[MIMPID].value) < 0)
		cpuid = NULL;

exit:
	return cpuid;
}

int get_cpuid(char *buffer, size_t sz)
{
	struct perf_cpu_map *cpus = perf_cpu_map__new(NULL);
	char *cpuid;
	int ret = 0;

	cpuid = _get_cpuid(cpus);

	if (sz < strlen(cpuid)) {
		ret = -EINVAL;
		goto free;
	}

	scnprintf(buffer, sz, "%s", cpuid);
free:
	perf_cpu_map__put(cpus);
	free(cpuid);
	return ret;
}

char *
get_cpuid_str(struct perf_pmu *pmu __maybe_unused)
{
	if (!pmu || !pmu->cpus)
		return NULL;
	return _get_cpuid(pmu->cpus);
}
