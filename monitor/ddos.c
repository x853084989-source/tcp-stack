/*
 * 沿用课程参考公式，对滑动统计窗口计算阈值分数；不是已验证的攻击分类器。
 * 这里只观察输入mbuf，不接管或释放它；命中阈值时保留现有退出进程策略。
 */

#include "ddos.h"

#include <math.h>
#include <stdlib.h>

#include <rte_eal.h>

/* Reference formula, including its length term; this is not a validated classifier. */
/* 保留参考公式及长度项；0/全0/全1分支避开log2(0)，不据此宣称检测效果已验证。 */
static double calculate_entropy(uint64_t set_bits, uint64_t total_bits) {
    if (total_bits == 0) {
        return 0.0;
    }

    double set = (double)set_bits;
    double total = (double)total_bits;
    double unset = total - set;
    double entropy = log2(total);
    if (set_bits != 0) {
        entropy -= set * (log2(set) - log2(total));
    }

    if (set_bits != total_bits) {
        entropy -= unset * (log2(unset) - log2(total));
    }

    return entropy;
}

/*
 * 借用可能含多个mbuf段的一帧，统计位数并写入环形样本窗口。
 * score由输出指针返回；函数返回1为超阈值、0为未超/未满窗口、-1为输入布局非法。
 */
int ddos_sample_packet(struct ddos_detector *detector, const struct rte_mbuf *mbuf,
                       double *score) {
    if (detector == NULL || mbuf == NULL || score == NULL) {
        return -1;
    }

    *score = 0.0;

    uint64_t set_bits = 0;
    uint64_t total_bits = 0;
    uint32_t remaining_length = rte_pktmbuf_pkt_len(mbuf);

    /* segment是同一帧的当前mbuf分段，不是TCP segment；沿next遍历时同步核对段数与总长度。 */
    const struct rte_mbuf *segment = mbuf;
    unsigned segment_count = 0;
    while (segment != NULL && segment_count < mbuf->nb_segs) {
        uint32_t data_length = rte_pktmbuf_data_len(segment);

        /* 先检查data_off及本段/总剩余长度，防止损坏mbuf元数据让位统计读出实际缓冲区范围。 */
        if (segment->data_off > segment->buf_len ||
            data_length > (uint32_t)segment->buf_len - segment->data_off ||
            data_length > remaining_length) {
            return -1;
        }

        const uint8_t *data = rte_pktmbuf_mtod(segment, const uint8_t *);
        for (uint32_t i = 0; i < data_length; i++) {
            set_bits += (unsigned)__builtin_popcount((unsigned)data[i]);
        }

        total_bits += (uint64_t)data_length * 8;
        remaining_length -= data_length;
        segment = segment->next;
        segment_count++;
    }

    if (remaining_length != 0 || segment != NULL || segment_count != mbuf->nb_segs) {
        return -1;
    }

    /* sample借用检测器环形数组的当前槽位，写满后覆盖最旧样本，不新分配节点。 */
    struct ddos_sample *sample = &detector->samples[detector->write_index];
    sample->set_bits = set_bits;
    sample->total_bits = total_bits;
    sample->entropy = calculate_entropy(set_bits, total_bits);
    detector->write_index = (detector->write_index + 1) % DDOS_WINDOW_SIZE;

    /* Match the reference: first evaluate on the packet after the window fills. */
    /* 沿用参考行为：收集满窗口的这一包仍返回0，下一包覆盖旧槽后才开始计算窗口分数。 */
    if (detector->sample_count < DDOS_WINDOW_SIZE) {
        detector->sample_count++;
        return 0;
    }

    uint64_t window_set_bits = 0;
    uint64_t window_total_bits = 0;
    double entropy_sum = 0.0;
    for (unsigned i = 0; i < DDOS_WINDOW_SIZE; i++) {
        window_set_bits += detector->samples[i].set_bits;
        window_total_bits += detector->samples[i].total_bits;
        entropy_sum += detector->samples[i].entropy;
    }

    *score = entropy_sum - calculate_entropy(window_set_bits, window_total_bits);
    if (*score > DDOS_THRESHOLD) {
        return 1;
    }

    return 0;
}

/* 由I/O线程调用；静态detector跨报文保留样本，沿用达到阈值就退出进程的现有策略。 */
void ddos_detect(struct rte_mbuf *mbuf) {
    static struct ddos_detector detector;
    double score;
    if (ddos_sample_packet(&detector, mbuf, &score) == 1) {
        rte_exit(EXIT_FAILURE, "Reference DDoS threshold exceeded: score=%.3f > %.3f\n",
                 score, DDOS_THRESHOLD);
    }
}
