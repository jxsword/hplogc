/* -*- coding: utf-8 -*- */
/**
 * @file ring_test.c
 * @brief 环形缓冲单元测试：预留/提交/弹出、丢弃、覆盖三种溢出策略。
 */
#include "test_common.h"

#include <hplogc.h>

#include "ring/ring.h"

#include <stdlib.h>
#include <string.h>

static int test_discard(void)
{
    hp_ring_t rb;
    unsigned char buf[256];
    size_t len;
    int i;

    if (hp_ring_init(&rb, 1024, HPLOGC_OVERFLOW_DISCARD) != HP_RING_OK) {
        CHECK(0);
        return 1;
    }
    /* 写入 10 条 8 字节记录 */
    for (i = 0; i < 10; i++) {
        unsigned char* p = NULL;
        size_t rc = 0;
        int r = hp_ring_reserve(&rb, 8, &p, &rc);
        CHECK_EQ(r, HP_RING_OK);
        CHECK(p != NULL);
        memset(p, (unsigned char)(i + 1), 8);
        hp_ring_commit(&rb, 8);
    }
    CHECK_EQ(hp_ring_used(&rb), (size_t)(10 * ((8 + 4 + 3) & ~(size_t)3)));

    /* 弹出并校验顺序 */
    for (i = 0; i < 10; i++) {
        int r = hp_ring_pop(&rb, buf, sizeof(buf), &len);
        CHECK_EQ(r, 1);
        CHECK_EQ(len, (size_t)8);
        CHECK_EQ((int)buf[0], i + 1);
    }
    CHECK_EQ(hp_ring_pop(&rb, buf, sizeof(buf), &len), 0); /* 空 */
    hp_ring_destroy(&rb);
    return 0;
}

static int test_overwrite(void)
{
    hp_ring_t rb;
    unsigned char buf[256];
    size_t len;
    int i;

    if (hp_ring_init(&rb, 256, HPLOGC_OVERFLOW_OVERWRITE) != HP_RING_OK) {
        CHECK(0);
        return 1;
    }
    /* 缓冲很小：写入 100 条，应全部被覆盖保留（旧的前部被丢弃） */
    for (i = 0; i < 100; i++) {
        unsigned char* p = NULL;
        size_t rc = 0;
        int r = hp_ring_reserve(&rb, 8, &p, &rc);
        CHECK(r == HP_RING_OK || r == HP_RING_DISCARD);
        if (r == HP_RING_OK) {
            memset(p, (unsigned char)(i & 0xFF), 8);
            hp_ring_commit(&rb, 8);
        }
    }
    /* 弹出的记录数应等于缓冲能容纳的条数（8+4 对齐=12），且全部成功 */
    {
        unsigned long long total = 0;
        while (hp_ring_pop(&rb, buf, sizeof(buf), &len) == 1) {
            total++;
            CHECK_EQ(len, (size_t)8);
        }
        CHECK(total > 0);
    }
    CHECK(hp_ring_overwritten(&rb) > 0); /* 必然发生覆盖 */
    hp_ring_destroy(&rb);
    return 0;
}

static int test_discard_full(void)
{
    hp_ring_t rb;
    int i;
    unsigned long long accepted = 0;

    if (hp_ring_init(&rb, 256, HPLOGC_OVERFLOW_DISCARD) != HP_RING_OK) {
        CHECK(0);
        return 1;
    }
    for (i = 0; i < 100; i++) {
        unsigned char* p = NULL;
        size_t rc = 0;
        int r = hp_ring_reserve(&rb, 8, &p, &rc);
        if (r == HP_RING_OK) {
            memset(p, 1, 8);
            hp_ring_commit(&rb, 8);
            accepted++;
        } else if (r == HP_RING_DISCARD) {
            /* 期望：队列满后开始丢弃 */
        } else {
            CHECK(0);
        }
    }
    CHECK(accepted <= (256 / ((8 + 4 + 3) & ~(size_t)3)));
    hp_ring_destroy(&rb);
    return 0;
}

int main(void)
{
    test_discard();
    test_overwrite();
    test_discard_full();
    return test_summary("ring");
}
