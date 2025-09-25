#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>

#define DEBUG_MSG(title, msg) plugin.DisplayUserMessage("LOA DEBUG", title, msg, true, true, false, false, false);

// Tagged/Untagged XFL Tag Item
void RenderXFLTagItem(
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

    if (flightPlan.GetState() != FLIGHT_PLAN_STATE_ASSUMED) return;

    const auto& fpd = flightPlan.GetFlightPlanData();
    if (_stricmp(fpd.GetPlanType(), "I") != 0) return;

    const auto& data = plugin.lastTagData;
    const std::string& origin = data.origin;
    const std::string& destination = data.destination;
    int clearedAltitude = data.clearedAltitude;
    int finalAltitude = data.finalAltitude;

    const auto& onlineControllers = plugin.GetOnlineControllersCached();
    const auto& routePoints = plugin.GetCachedRoutePoints(flightPlan);

    //COORDINATION LOGIC.
    std::string callsign = flightPlan.GetCallsign();
    int coordXFL = flightPlan.GetExitCoordinationAltitude();
    int coordState = flightPlan.GetExitCoordinationAltitudeState();

    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && coordXFL >= 500) {
        plugin.coordinationStates[callsign].exitAltitude = coordXFL;
        plugin.coordinationStates[callsign].exitAltitudeState = COORDINATION_STATE_REQUESTED_BY_ME;
    }

    if (coordState == COORDINATION_STATE_NONE) {
        const auto it = plugin.coordinationStates.find(callsign);
        if (it != plugin.coordinationStates.end()) {
            const auto& info = it->second;
            if (info.exitAltitude >= 500 && info.exitAltitude == coordXFL && info.exitAltitudeState == COORDINATION_STATE_REQUESTED_BY_ME) {
                snprintf(sItemString, 16, "%03d", coordXFL / 100);
                return;
            }
        }
    }

    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REFUSED) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (entry.requireNextSectorOnline) {
            if (std::none_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                [&](const std::string& s) { return onlineControllers.count(s); }))
                return false;
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

    auto tryLOA = [&](const std::vector<LOAEntry>& list, bool belowXFL = true) -> bool {
        for (const auto& entry : list) {
            if (!matches(entry)) continue;

            if (belowXFL && clearedAltitude < entry.xfl * 100 && finalAltitude > entry.xfl * 100) {
                snprintf(sItemString, 16, "%d", entry.xfl);
                return true;
            }
            else if (!belowXFL && clearedAltitude > entry.xfl * 100) {
                snprintf(sItemString, 16, "%d", entry.xfl);
                return true;
            }
            else if (clearedAltitude == entry.xfl * 100 || clearedAltitude == finalAltitude) {
                sItemString[0] = 0;
                return true;
            }
            else if (clearedAltitude != finalAltitude) {
                snprintf(sItemString, 16, "%d", finalAltitude / 100);
                return true;
            }
        }
        return false;
        };

    if (tryLOA(departureLoas, true)) return;
    if (tryLOA(destinationLoas, false)) return;
    if (tryLOA(lorDepartures, true)) return;
    if (tryLOA(lorArrivals, false)) return;

    if (clearedAltitude == finalAltitude) {
        sItemString[0] = 0;
    }
    else {
        snprintf(sItemString, 16, "%d", finalAltitude / 100);
    }
}

// ✅ Optimized Detailed Tag — only 1 route extract
// =========================
// File: TagXFL.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <string>
#include <algorithm>

#define DEBUG_MSG(title, msg) plugin.DisplayUserMessage("LOA DEBUG", title, msg, true, true, false, false, false);

// ✅ Optimized Detailed Tag — only 1 route extract
void RenderXFLDetailedTagItem(
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

    if (!flightPlan.IsValid() || !plugin.IsLOARelevantState(flightPlan.GetState())) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
        return;
    }

    const auto& fpd = flightPlan.GetFlightPlanData();
    const char* planType = fpd.GetPlanType();
    if (_stricmp(planType, "I") != 0) {
        strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
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

    //COORDINATION LOGIC.
    int coordXFL = flightPlan.GetExitCoordinationAltitude();
    int coordState = flightPlan.GetExitCoordinationAltitudeState();

    if ((coordState == COORDINATION_STATE_REQUESTED_BY_ME || coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) && coordXFL >= 500) {
        plugin.coordinationStates[callsign].exitAltitude = coordXFL;
        plugin.coordinationStates[callsign].exitAltitudeState = COORDINATION_STATE_REQUESTED_BY_ME;
    }

    if (coordState == COORDINATION_STATE_NONE) {
        const auto it = plugin.coordinationStates.find(callsign);
        if (it != plugin.coordinationStates.end()) {
            const auto& info = it->second;
            if (info.exitAltitude >= 500 && info.exitAltitude == coordXFL && info.exitAltitudeState == COORDINATION_STATE_REQUESTED_BY_ME) {
                snprintf(sItemString, 16, "%03d", coordXFL / 100);
                *pColorCode = TAG_COLOR_ONGOING_REQUEST_ACCEPTED;
                return;
            }
        }
    }

    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_ME) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_FROM_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REQUESTED_BY_OTHER) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_TO_ME;
        return;
    }
    if (coordXFL >= 500 && coordState == COORDINATION_STATE_REFUSED) {
        snprintf(sItemString, 16, "%03d", coordXFL / 100);
        *pColorCode = TAG_COLOR_ONGOING_REQUEST_REFUSED;
        return;
    }

    auto matches = [&](const LOAEntry& entry) -> bool {
        if (entry.requireNextSectorOnline) {
            bool nextOnline = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                [&](const std::string& s) {
                    return onlineControllers.count(s) > 0;
                });
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
            if (clearedAltitude <= entry.xfl * 100 && finalAltitude > entry.xfl * 100) {
                strncpy_s(sItemString, 16, std::to_string(entry.xfl).c_str(), _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    for (const auto& entry : destinationLoas) {
        if (matches(entry)) {
            if (clearedAltitude < entry.xfl * 100) {
                strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, std::to_string(entry.xfl).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    for (const auto& entry : lorDepartures) {
        if (matches(entry)) {
            if (clearedAltitude <= entry.xfl * 100 && finalAltitude > entry.xfl * 100) {
                strncpy_s(sItemString, 16, std::to_string(entry.xfl).c_str(), _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    for (const auto& entry : lorArrivals) {
        if (matches(entry)) {
            if (clearedAltitude < entry.xfl * 100) {
                strncpy_s(sItemString, 16, "XFL", _TRUNCATE);
                return;
            }
            else {
                strncpy_s(sItemString, 16, std::to_string(entry.xfl).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    if (!fallbackLoas.empty()) {
        for (const auto& entry : fallbackLoas) {
            if (clearedAltitude < entry.minAltitudeFt) continue;

            if (!entry.destinationAirports.empty() &&
                !plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination)) {
                continue;
            }

            bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
                [&](const std::string& wp) {
                    return std::any_of(routePoints.begin(), routePoints.end(),
                        [&](const std::string& r) { return _stricmp(r.c_str(), wp.c_str()) == 0; });
                });

            if (wpMatch) {
                strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
                return;
            }
        }
    }

    strncpy_s(sItemString, 16, std::to_string(finalAltitude / 100).c_str(), _TRUNCATE);
}