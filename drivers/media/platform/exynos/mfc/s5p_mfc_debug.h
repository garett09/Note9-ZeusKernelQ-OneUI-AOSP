/*
 * drivers/media/platform/exynos/mfc/s5p_mfc_debug.h
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __S5P_MFC_DEBUG_H
#define __S5P_MFC_DEBUG_H __FILE__

//#define DEBUG

extern unsigned int debug_level;
extern unsigned int debug_ts;
extern unsigned int dbg_enable;
extern unsigned int nal_q_dump;
extern unsigned int nal_q_disable;
extern unsigned int nal_q_parallel_disable;
extern unsigned int otf_dump;

#ifdef DEBUG

#define mfc_debug(level, fmt, args...)				\
	do {							\
		if (debug_level >= level)				\
			printk(KERN_DEBUG "%s:%d: " fmt,	\
				__func__, __LINE__, ##args);	\
	} while (0)
#else
#define mfc_debug(fmt, args...)
#endif

#define mfc_debug_enter() mfc_debug(5, "enter\n")
#define mfc_debug_leave() mfc_debug(5, "leave\n")

#define mfc_err_dev(fmt, args...)

#define mfc_err_ctx(fmt, args...)

#define mfc_info_dev(fmt, args...)

#define mfc_info_ctx(fmt, args...)

#define MFC_TRACE_STR_LEN		80
#define MFC_TRACE_COUNT_MAX		1024
#define MFC_TRACE_COUNT_PRINT		30
#define MFC_TRACE_LOG_STR_LEN		25
#define MFC_TRACE_LOG_COUNT_MAX		256
#define MFC_TRACE_LOG_COUNT_PRINT	20

struct _mfc_trace {
	unsigned long long time;
	char str[MFC_TRACE_STR_LEN];
};

struct _mfc_trace_logging {
	unsigned long long time;
	char str[MFC_TRACE_LOG_STR_LEN];
};

/* If there is no ctx structure */
#define MFC_TRACE_DEV(fmt, args...)

/* If there is ctx structure */
#define MFC_TRACE_CTX(fmt, args...)


/* If there is no ctx structure */
#define MFC_TRACE_DEV_LT(fmt, args...)

/* If there is ctx structure */
#define MFC_TRACE_CTX_LT(fmt, args...)

/* If there is no ctx structure */
#define MFC_TRACE_LOG_DEV(fmt, args...)

/* If there is ctx structure */
#define MFC_TRACE_LOG_CTX(fmt, args...)


#endif /* __S5P_MFC_DEBUG_H */
