/*
 * Linux内核诊断工具--用户态alloc-top功能实现
 *
 * Copyright (C) 2020 Alibaba Ltd.
 *
 * 作者: Baoyou Xie <baoyou.xie@linux.alibaba.com>
 *
 * License terms: GNU General Public License (GPL) version 3
 *
 */

#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <sys/time.h>
#include <string.h>
#include <stdio.h>     /* for printf */
#include <stdlib.h>    /* for exit */

#include "internal.h"
#include "symbol.h"
#include "uapi/alloc_top.h"
#include "params_parse.h"

static char sls_file[256];
static int syslog_enabled;

void usage_alloc_top(void)
{
	printf("    alloc-top usage:\n");
	printf("        --help alloc-top help info\n");
	printf("        --activate\n");
	printf("          verbose VERBOSE\n");
	printf("          top max count in top list\n");
	printf("        --deactivate\n");
	printf("        --report dump log with text.\n");
	printf("        --settins dump settings with text.\n");
	printf("        --test testcase for alloc-top.\n");
	printf("        --log\n");
	printf("          sls=/tmp/1.log store in file\n");
	printf("          syslog=1 store in syslog\n");
}

static void do_activate(const char *arg)
{
	int ret = 0;
	struct params_parser parse(arg);
	struct diag_alloc_top_settings settings;

	memset(&settings, 0, sizeof(struct diag_alloc_top_settings));
	
	settings.top = parse.int_value("top");
	if (settings.top <= 0)
		settings.top = 20;
	settings.verbose = parse.int_value("verbose");

	if (run_in_host) {
		ret = diag_call_ioctl(DIAG_IOCTL_ALLOC_TOP_SET, (long)&settings);
	} else {
		ret = -ENOSYS;
		syscall(DIAG_ALLOC_TOP_SET, &ret, &settings, sizeof(struct diag_alloc_top_settings));
	}

	printf("功能设置%s，返回值：%d\n", ret ? "失败" : "成功", ret);
	printf("    TOP-N：%d\n", settings.top);
	printf("    输出级别：%d\n", settings.verbose);

	if (ret)
		return;

	ret = diag_activate("alloc-top");
	if (ret == 1) {
		printf("alloc-top activated\n");
	} else {
		printf("alloc-top is not activated, ret %d\n", ret);
	}
}

static void do_deactivate(void)
{
	int ret = 0;

	ret = diag_deactivate("alloc-top");
	if (ret == 0) {
		printf("alloc-top is not activated\n");
	} else {
		printf("deactivate alloc-top fail, ret is %d\n", ret);
	}
}

static void print_settings_in_json(struct diag_alloc_top_settings *settings, int ret)
{
	Json::Value root;
	std::string str_log;

	if (ret == 0) {
		root["activated"] = Json::Value(settings->activated);
		root["TOP-N"] = Json::Value(settings->top);
		root["verbose"] = Json::Value(settings->verbose);
	} else {
		root["err"] = Json::Value("found alloc-top settings failed, please check if diagnose-tools is installed correctly or not.");
	}

	str_log.append(root.toStyledString());
	printf("%s", str_log.c_str());

	return;
}

static void do_settings(const char *arg)
{
	struct diag_alloc_top_settings settings;
	int ret;
	int enable_json = 0;
	struct params_parser parse(arg);
	enable_json = parse.int_value("json");

	if (run_in_host) {
		ret = diag_call_ioctl(DIAG_IOCTL_ALLOC_TOP_SETTINGS, (long)&settings);
	} else {
		ret = -ENOSYS;
		syscall(DIAG_ALLOC_TOP_SETTINGS, &ret, &settings, sizeof(struct diag_alloc_top_settings));
	}

	if (1 == enable_json) {
		return print_settings_in_json(&settings, ret);
	}

	if (ret == 0) {
		printf("功能设置：\n");
		printf("    是否激活：%s\n", settings.activated ? "√" : "×");
		printf("    TOP-N：%d\n", settings.top);
		printf("    输出级别：%d\n", settings.verbose);
	} else {
		printf("获取alloc-top设置失败，请确保正确安装了diagnose-tools工具\n");
	}
}

static int alloc_top_extract(void *buf, unsigned int len, void *)
{
	int *et_type;
	struct alloc_top_detail *detail;

	if (len == 0)
		return 0;

	et_type = (int *)buf;
	printf("  序号     TGID                COMM    PG-COUNT              CGROUP\n");
	switch (*et_type) {
	case et_alloc_top_detail:
		if (len < sizeof(struct alloc_top_detail))
			break;
		detail = (struct alloc_top_detail *)buf;
		printf("%5d%10lu%20s%12lu%32s\n",
			detail->seq, detail->tgid, detail->comm, detail->page_count, detail->cgroup_name);
		break;
	default:
		break;
	}

	return 0;
}

static int sls_extract(void *buf, unsigned int len, void *)
{
	int *et_type;
	struct alloc_top_detail *detail;
	Json::Value root;
	struct timeval tv;

	if (len == 0)
		return 0;

	et_type = (int *)buf;
	switch (*et_type) {
	case et_alloc_top_detail:
		if (len < sizeof(struct alloc_top_detail))
			break;
		detail = (struct alloc_top_detail *)buf;
		root["seq"] = Json::Value(detail->seq);
		root["tgid"] = Json::Value(detail->tgid);
		root["comm"] = Json::Value(detail->comm);
		root["page_count"] = Json::Value(detail->page_count);
		root["cgroup_name"] = Json::Value(detail->cgroup_name);

		gettimeofday(&tv, NULL);
		write_file(sls_file, "alloc-top", &tv, detail->id, detail->seq, root);
		write_syslog(syslog_enabled, "alloc-top", &tv, detail->id, detail->seq, root);
		break;
	default:
		break;
	}

	return 0;
}

static void do_extract(char *buf, int len)
{
	extract_variant_buffer(buf, len, alloc_top_extract, NULL);
}

static void do_dump(void)
{
	static char variant_buf[1024 * 1024];
	int len;
	int ret = 0;
	struct diag_ioctl_dump_param dump_param = {
		.user_ptr_len = &len,
		.user_buf_len = 1024 * 1024,
		.user_buf = variant_buf,
	};

	memset(variant_buf, 0, 1024 * 1024);
	if (run_in_host) {
		ret = diag_call_ioctl(DIAG_IOCTL_ALLOC_TOP_DUMP, (long)&dump_param);
	} else {
		ret = -ENOSYS;
		syscall(DIAG_ALLOC_TOP_DUMP, &ret, &len, variant_buf, 1024 * 1024);
	}

	if (ret == 0) {
		do_extract(variant_buf, len);
	}
}

static void do_sls(char *arg)
{
	int ret;
	static char variant_buf[1024 * 1024];

	int len;
	struct diag_ioctl_dump_param dump_param = {
		.user_ptr_len = &len,
		.user_buf_len = 1024 * 1024,
		.user_buf = variant_buf,
	};

	ret = log_config(arg, sls_file, &syslog_enabled);
	if (ret != 1)
		return;

	while (1) {
		if (run_in_host) {
			ret = diag_call_ioctl(DIAG_IOCTL_ALLOC_TOP_DUMP, (long)&dump_param);
		} else {
			syscall(DIAG_ALLOC_TOP_DUMP, &ret, &len, variant_buf, 1024 * 1024);
		}

		if (ret == 0 && len > 0) {
			extract_variant_buffer(variant_buf, len, sls_extract, NULL);
		}

		sleep(10);
	}	

}

int alloc_top_main(int argc, char **argv)
{
	static struct option long_options[] = {
			{"help",     no_argument, 0,  0 },
			{"activate",     optional_argument, 0,  0 },
			{"deactivate", no_argument,       0,  0 },
			{"settings",     optional_argument, 0,  0 },
			{"report",     no_argument, 0,  0 },
			{"log",     required_argument, 0,  0 },
			{0,         0,                 0,  0 }
		};
	int c;

	if (argc <= 1) {
		usage_alloc_top();
		return 0;
	}
	while (1) {
		int option_index = -1;

		c = getopt_long_only(argc, argv, "", long_options, &option_index);
		if (c == -1)
			break;
		switch (option_index) {
		case 0:
			usage_alloc_top();
			break;
	    case 1:
			do_activate(optarg ? optarg : "");
			break;
		case 2:
			do_deactivate();
			break;
		case 3:
			do_settings(optarg ? optarg : "");
			break;
		case 4:
			do_dump();
			break;
		case 5:
			do_sls(optarg);
			break;
		default:
			usage_alloc_top();
			break;
		}
	}

	return 0;
}
