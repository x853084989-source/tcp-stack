/* 检测器保存最近若干帧的统计样本，这个统计窗口与TCP滑动窗口无关。 */

#ifndef MYPROGRAM_DDOS_H
#define MYPROGRAM_DDOS_H

#include <stdint.h>
#include <rte_mbuf.h>

#define DDOS_WINDOW_SIZE 256
#define DDOS_THRESHOLD 1200.0

struct ddos_sample {
    uint64_t set_bits;
    uint64_t total_bits;
    double entropy;
};

struct ddos_detector {
    struct ddos_sample samples[DDOS_WINDOW_SIZE];

    /* 下一次覆盖的样本槽位，按DDOS_WINDOW_SIZE取模，不是TCP接收序号。 */
    unsigned write_index;

    /* 已收集样本数，达到窗口大小后保持饱和，控制何时开始给出分数。 */
    unsigned sample_count;
};

/* Borrows all packet segments; returns 1 above threshold, 0 below/not full, -1 invalid. */
/* 借用可能含多个mbuf段的一帧，统计位数并写入环形样本窗口。 */
int ddos_sample_packet(struct ddos_detector *detector, const struct rte_mbuf *mbuf,
                       double *score);

/* RX I/O owner: retains the reference's threshold-triggered process exit. */
/* 由I/O线程调用；静态detector跨报文保留样本，沿用达到阈值就退出进程的现有策略。 */
void ddos_detect(struct rte_mbuf *mbuf);

#endif /* MYPROGRAM_DDOS_H */
