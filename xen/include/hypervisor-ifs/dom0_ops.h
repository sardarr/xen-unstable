/******************************************************************************
 * dom0_ops.h
 * 
 * Process command requests from domain-0 guest OS.
 * 
 * Copyright (c) 2002-2003, K A Fraser, B Dragovic
 */


#ifndef __DOM0_OPS_H__
#define __DOM0_OPS_H__

#define DOM0_GETMEMLIST     2
#define DOM0_BVTCTL         6
#define DOM0_ADJUSTDOM      7
#define DOM0_CREATEDOMAIN   8
#define DOM0_DESTROYDOMAIN  9
#define DOM0_STARTDOMAIN   10
#define DOM0_STOPDOMAIN    11
#define DOM0_GETDOMAININFO 12
#define DOM0_BUILDDOMAIN   13
#define DOM0_IOPL          14
#define DOM0_MSR           15
#define DOM0_DEBUG         16
#define DOM0_SETTIME       17

#define MAX_CMD_LEN       256
#define MAX_DOMAIN_NAME    16

typedef struct dom0_newdomain_st 
{
    /* IN parameters. */
    unsigned int memory_kb; 
    char name[MAX_DOMAIN_NAME];
    /* OUT parameters. */
    unsigned int domain; 
} dom0_newdomain_t;

typedef struct dom0_killdomain_st
{
    unsigned int domain;
    int          force;
} dom0_killdomain_t;

typedef struct dom0_getmemlist_st
{
    /* IN variables. */
    unsigned int  domain;
    unsigned long max_pfns;
    void         *buffer;
    /* OUT variables. */
    unsigned long num_pfns;
} dom0_getmemlist_t;

typedef struct domain_launch
{
    unsigned int  domain;
    unsigned long l2_pgt_addr;
    unsigned long virt_load_addr;
    unsigned long virt_startinfo_addr;
    unsigned int num_vifs;
    char cmd_line[MAX_CMD_LEN];
    unsigned long virt_mod_addr;
    unsigned long virt_mod_len;
} dom_meminfo_t;

typedef struct dom0_bvtctl_st
{
    unsigned long ctx_allow;	/* context switch allowance */
} dom0_bvtctl_t;

typedef struct dom0_adjustdom_st
{
    unsigned int  domain;	/* domain id */
    unsigned long mcu_adv;	/* mcu advance: inverse of weight */
    unsigned long warp;     /* time warp */
    unsigned long warpl;    /* warp limit */
    unsigned long warpu;    /* unwarp time requirement */
} dom0_adjustdom_t;

typedef struct dom0_getdominfo_st
{
    /* IN variables. */
    unsigned int domain;
    /* OUT variables. */
    char name[MAX_DOMAIN_NAME];
    int processor;
    int has_cpu;
    int state;
    int hyp_events;
    unsigned long mcu_advance;
    unsigned int tot_pages;
    long long cpu_time;
} dom0_getdominfo_t;

typedef struct dom0_iopl_st
{
    unsigned int domain;
    unsigned int iopl;
} dom0_iopl_t;

typedef struct dom0_msr_st
{
    /* IN variables. */
    int write, cpu_mask, msr;
    unsigned int in1, in2;
    /* OUT variables. */
    unsigned int out1, out2;
} dom0_msr_t;

typedef struct dom0_debug_st
{
    /* IN variables. */
    char opcode;
    int domain, in1, in2;
    /* OUT variables. */
    int status, out1, out2;
} dom0_debug_t;

/*
 * Set clock such that it would read <secs,usecs> after 00:00:00 UTC,
 * 1 January, 1970 if the current system time was <system_time>.
 */
typedef struct dom0_settime_st
{
    /* IN variables. */
    unsigned long secs, usecs;
    u64 system_time;
} dom0_settime_t;

typedef struct dom0_op_st
{
    unsigned long cmd;
    union
    {
        dom0_newdomain_t newdomain;
        dom0_killdomain_t killdomain;
        dom0_getmemlist_t getmemlist;
        dom0_bvtctl_t bvtctl;
        dom0_adjustdom_t adjustdom;
        dom_meminfo_t meminfo;
        dom0_getdominfo_t getdominfo;
        dom0_iopl_t iopl;
	dom0_msr_t msr;
	dom0_debug_t debug;
	dom0_settime_t settime;
    }
    u;
} dom0_op_t;

#endif
