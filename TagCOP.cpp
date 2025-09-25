// =========================
// File: TagCOP.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <cstring>
#include <windows.h>
#include <algorithm>

void RenderCOPTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    if (!radarTarget.IsValid()) return;

    EuroScopePlugIn::CFlightPlan correlated = radarTarget.GetCorrelatedFlightPlan();
    if (!correlated.IsValid()) return;

    if (!plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    const char* planType = fpd.GetPlanType();
    if (_stricmp(planType, "I") != 0) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        return;
    }

    const auto& data = plugin.lastTagData;
    const std::string& callsign = data.callsign;
    int clearedAltitude = data.clearedAltitude;
    int finalAltitude = data.finalAltitude;
    const std::string& origin = data.origin;
    const std::string& destination = data.destination;

    const auto& onlineControllers = plugin.currentFrameOnlineControllers;
    const auto& routePoints = plugin.currentFrameRoutePoints;

    // COORDINATION LOGIC
    std::string coordCOP = flightPlan.GetExitCoordinationPointName();
    int coordState = flightPlan.GetExitCoordinationNameState();

    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && !coordCOP.empty()) {
        plugin.coordinationStates[callsign].exitPoint = coordCOP;
        plugin.coordinationStates[callsign].exitPointState = COORDINATION_STATE_REQUESTED_BY_ME;
    }

    if (coordState == COORDINATION_STATE_NONE) {
        auto& info = plugin.coordinationStates[callsign];
        if (!info.exitPoint.empty() &&
            info.exitPoint == coordCOP &&
            (info.exitPointState == COORDINATION_STATE_REQUESTED_BY_ME || info.exitPointState == COORDINATION_STATE_REQUESTED_BY_OTHER)) {
            info.exitPointState = COORDINATION_STATE_MANUAL_ACCEPTED;
        }
    }

    const auto it = plugin.coordinationStates.find(callsign);
    if (it != plugin.coordinationStates.end()) {
        const auto& info = it->second;
        if (info.exitPointState == COORDINATION_STATE_MANUAL_ACCEPTED && !info.exitPoint.empty()) {
            strncpy_s(sItemString, 16, info.exitPoint.c_str(), _TRUNCATE);
            *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
            return;
        }
    }

    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        strncpy_s(sItemString, 16, coordCOP.c_str(), _TRUNCATE);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (!coordCOP.empty() && coordState == COORDINATION_STATE_REFUSED) {
        strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (entry.requireNextSectorOnline) {
            bool nextOnline = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                [&](const std::string& s) { return onlineControllers.count(s) > 0; });
            if (!nextOnline) return false;
        }

        bool originMatch = entry.originAirports.empty() || plugin.MatchesAirport(entry.originAirportSet, entry.originAirportPrefixes, origin);
        bool destMatch = entry.destinationAirports.empty() || plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination);

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
            [&](const std::string& wp) {
                return std::any_of(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
            });

        return originMatch && destMatch && wpMatch;
        };

    for (const auto& entry : departureLoas) {
        if (matches(entry)) {
            if (clearedAltitude <= entry.xfl * 100) {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
            else {
                break;
            }
        }
    }

    for (const auto& entry : destinationLoas) {
        if (matches(entry)) {
            if (clearedAltitude >= entry.xfl * 100) {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
            else {
                break;
            }
        }
    }

    for (const auto& entry : lorDepartures) {
        if (matches(entry)) {
            if (clearedAltitude <= entry.xfl * 100) {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
            else {
                break;
            }
        }
    }

    for (const auto& entry : lorArrivals) {
        if (matches(entry)) {
            if (clearedAltitude >= entry.xfl * 100) {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
            else {
                break;
            }
        }
    }

    if (!fallbackLoas.empty()) {
        for (const auto& entry : fallbackLoas) {
            if (clearedAltitude < entry.minAltitudeFt) continue;
            if (!entry.destinationAirports.empty() &&
                !plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination)) continue;

            bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
                [&](const std::string& wp) {
                    return std::any_of(routePoints.begin(), routePoints.end(),
                        [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
                });

            if (wpMatch) {
                strncpy_s(sItemString, 16, entry.copText.c_str(), _TRUNCATE);
                return;
            }
        }
    }

    strncpy_s(sItemString, 16, "COPX", _TRUNCATE);
}