#ifndef SENDER_CONFIG_H
#define SENDER_CONFIG_H

#include "dosfer.h"

void config_defaults(Config *cfg);
int config_validate(const Config *cfg);

u16 config_redundancy_group(const Config *cfg);
const char *config_redundancy_name(const Config *cfg);
const char *config_video_name(const Config *cfg);

int config_parse_option(Config *cfg,const char *arg);
int config_parse_re(Config *cfg,const char *value);
int config_parse_video(Config *cfg,const char *value);
int config_is_split_re(const char *arg);
int config_is_split_video(const char *arg);

void config_print_usage(const Config *cfg);

#endif
