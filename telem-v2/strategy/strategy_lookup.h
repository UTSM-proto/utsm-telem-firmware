#pragma once

#include <Arduino.h>

#include "strategy_indy.h"

struct StrategyRecommendation {
  bool valid;
  uint16_t index;
  uint16_t segment;
  float targetSpeedKph;
  float distanceFromTrackM;
  StrategyAction action;
};

void resetStrategyLookup();
StrategyRecommendation getRecommendedSpeed(double latitude, double longitude);
const char *strategyActionName(StrategyAction action);
