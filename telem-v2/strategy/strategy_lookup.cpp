#include "strategy_lookup.h"

#include <math.h>

#if defined(ARDUINO_ARCH_AVR)
#include <avr/pgmspace.h>
#endif

static const int16_t SEARCH_WINDOW_POINTS = 24;
static int32_t g_lastStrategyIndex = -1;

static StrategyPoint readStrategyPoint(uint16_t index) {
  StrategyPoint point;
#if defined(ARDUINO_ARCH_AVR)
  memcpy_P(&point, &STRATEGY_POINTS[index], sizeof(StrategyPoint));
#else
  point = STRATEGY_POINTS[index];
#endif
  return point;
}

static int32_t latitudeToYcm(double latitude) {
  const double yM = (latitude - STRATEGY_LAT0) * STRATEGY_METERS_PER_DEG_LAT;
  return (int32_t)lround(yM * 100.0);
}

static int32_t longitudeToXcm(double longitude) {
  const double xM = (longitude - STRATEGY_LON0) * STRATEGY_METERS_PER_DEG_LON;
  return (int32_t)lround(xM * 100.0);
}

static uint16_t nearestIndexInRange(
  int32_t xCm,
  int32_t yCm,
  uint16_t startIndex,
  uint16_t endIndex,
  uint64_t *bestDistanceCm2
) {
  uint16_t bestIndex = startIndex;
  *bestDistanceCm2 = UINT64_MAX;

  for (uint16_t i = startIndex; i < endIndex; ++i) {
    StrategyPoint point = readStrategyPoint(i);
    int64_t dx = (int64_t)xCm - point.x_cm;
    int64_t dy = (int64_t)yCm - point.y_cm;
    uint64_t d2 = (uint64_t)(dx * dx + dy * dy);
    if (d2 < *bestDistanceCm2) {
      *bestDistanceCm2 = d2;
      bestIndex = i;
    }
  }

  return bestIndex;
}

static uint16_t nearestStrategyIndex(int32_t xCm, int32_t yCm, uint64_t *bestDistanceCm2) {
  if (STRATEGY_POINT_COUNT == 0) {
    *bestDistanceCm2 = UINT64_MAX;
    return 0;
  }

  if (g_lastStrategyIndex >= 0 && g_lastStrategyIndex < STRATEGY_POINT_COUNT) {
    int32_t start = g_lastStrategyIndex - SEARCH_WINDOW_POINTS;
    int32_t end = g_lastStrategyIndex + SEARCH_WINDOW_POINTS + 1;
    if (start < 0) start = 0;
    if (end > STRATEGY_POINT_COUNT) end = STRATEGY_POINT_COUNT;
    uint16_t startIndex = (uint16_t)start;
    uint16_t endIndex = (uint16_t)end;
    uint16_t localBest = nearestIndexInRange(xCm, yCm, startIndex, endIndex, bestDistanceCm2);
    float localDistanceM = sqrt((double)*bestDistanceCm2) / 100.0f;
    if (localDistanceM <= STRATEGY_OFFTRACK_RADIUS_M) {
      return localBest;
    }
  }

  return nearestIndexInRange(xCm, yCm, 0, STRATEGY_POINT_COUNT, bestDistanceCm2);
}

void resetStrategyLookup() {
  g_lastStrategyIndex = -1;
}

StrategyRecommendation getRecommendedSpeed(double latitude, double longitude) {
  StrategyRecommendation rec = {};
  rec.valid = false;
  rec.index = 0;
  rec.segment = 0;
  rec.targetSpeedKph = 0.0f;
  rec.distanceFromTrackM = INFINITY;
  rec.action = STRATEGY_UNKNOWN;

  if (!isfinite(latitude) || !isfinite(longitude) || STRATEGY_POINT_COUNT == 0) {
    resetStrategyLookup();
    return rec;
  }

  uint64_t bestDistanceCm2 = UINT64_MAX;
  uint16_t bestIndex = nearestStrategyIndex(
    longitudeToXcm(longitude),
    latitudeToYcm(latitude),
    &bestDistanceCm2
  );
  StrategyPoint point = readStrategyPoint(bestIndex);

  rec.index = bestIndex;
  rec.segment = point.segment;
  rec.targetSpeedKph = ((float)point.target_speed_kph_x10) / 10.0f;
  rec.distanceFromTrackM = sqrt((double)bestDistanceCm2) / 100.0f;
  rec.action = (StrategyAction)point.action;
  rec.valid = rec.distanceFromTrackM <= STRATEGY_OFFTRACK_RADIUS_M;

  if (rec.valid) {
    g_lastStrategyIndex = bestIndex;
  }

  return rec;
}

const char *strategyActionName(StrategyAction action) {
  switch (action) {
    case STRATEGY_ACCELERATE:
      return "accelerate";
    case STRATEGY_HOLD:
      return "hold";
    case STRATEGY_COAST:
      return "coast";
    default:
      return "unknown";
  }
}
