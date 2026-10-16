#include <math.h>
#include <string.h>
#include "ride_model.h"

/* 只累计运行区间，重复开始/暂停是幂等操作，与联网和系统日期无关。 */
int64_t ride_model_elapsed(const ride_model_t *model, int64_t now_us)
{
    return model->elapsed_us +
        (model->running && now_us >= model->started_us ? now_us - model->started_us : 0);
}

void ride_model_start(ride_model_t *model, int64_t now_us)
{
    if (!model->running) {
        model->started_us = now_us;
        model->running = true;
    }
}

void ride_model_pause(ride_model_t *model, int64_t now_us)
{
    model->elapsed_us = ride_model_elapsed(model, now_us);
    model->running = false;
}

void ride_model_reset(ride_model_t *model)
{
    memset(model, 0, sizeof(*model));
}

bool ride_model_sample(ride_model_t *model, const ride_sample_t *sample, int64_t now_us)
{
    if (!sample || !sample->valid) {
        model->sample.valid = false;
        return false;
    }
    if (!isfinite(sample->speed_kmh) || sample->speed_kmh < 0 || sample->speed_kmh > 200 ||
        !isfinite(sample->trip_km) || sample->trip_km < 0 ||
        (sample->altitude_valid && !isfinite(sample->altitude_m))) {
        return false;
    }
    model->sample = *sample;
    model->sample_us = now_us;
    if (model->running && now_us >= model->started_us && sample->speed_kmh > model->maximum_kmh) {
        model->maximum_kmh = sample->speed_kmh;
    }
    return true;
}

/* 五秒未收到真实数据后隐藏实时读数，不把断开的传感器显示为零速度。 */
bool ride_model_sample_fresh(const ride_model_t *model, int64_t now_us)
{
    return model->sample.valid && now_us >= model->sample_us &&
           now_us - model->sample_us <= 5000000;
}