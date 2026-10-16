#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 模型不操作 GPIO；未来 GPS/轮速驱动提供本次骑行累计距离及有效速度。 */
typedef struct {
    bool valid;
    float speed_kmh;
    float trip_km;
    float altitude_m;
    bool altitude_valid;
} ride_sample_t;

typedef struct {
    bool running;
    int64_t elapsed_us;
    int64_t started_us;
    int64_t sample_us;
    float maximum_kmh;
    ride_sample_t sample;
} ride_model_t;

void ride_model_start(ride_model_t *model, int64_t now_us);
void ride_model_pause(ride_model_t *model, int64_t now_us);
void ride_model_reset(ride_model_t *model);
int64_t ride_model_elapsed(const ride_model_t *model, int64_t now_us);
bool ride_model_sample(ride_model_t *model, const ride_sample_t *sample, int64_t now_us);
bool ride_model_sample_fresh(const ride_model_t *model, int64_t now_us);