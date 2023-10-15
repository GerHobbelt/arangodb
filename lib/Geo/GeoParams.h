////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Assertions/Assert.h"
#include "Geo/ShapeContainer.h"

#include <s2/s2latlng.h>
#include <s2/s2region_coverer.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <limits>

namespace arangodb {
namespace velocypack {

class Builder;
class Slice;

}  // namespace velocypack
namespace geo {

// assume up to 8x machine epsilon in precision errors for radian calculations
inline constexpr double kRadEps = 8 * std::numeric_limits<double>::epsilon();
inline constexpr double kMaxRadiansBetweenPoints = M_PI + kRadEps;
// Equatorial radius of earth.
// Source: http://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
// Equatorial radius
// inline constexpr double kEarthRadiusInMeters = 6'378'137.0;
// Volumetric mean radius
inline constexpr double kEarthRadiusInMeters = 6'371'000.0;
inline constexpr double kMaxDistanceBetweenPoints =
    kMaxRadiansBetweenPoints * kEarthRadiusInMeters;

constexpr double metersToRadians(double distanceInMeters) noexcept {
  return std::clamp(distanceInMeters / kEarthRadiusInMeters, 0.0, M_PI);
}

enum class FilterType {
  // no filter, only useful on a near query
  NONE,
  // Select documents with geospatial data that are located entirely
  // within a shape.
  // When determining inclusion, we consider the border of a shape to
  // be part of the shape,
  // subject to the precision of floating point numbers.
  CONTAINS,
  // Select documents whose geospatial data intersects with a
  // specified GeoJSON object.
  INTERSECTS
};

/// @brief contains parameters for s2 region coverer
struct RegionCoverParams {
  RegionCoverParams();
  RegionCoverParams(int mC, int wL, int bL)
      : maxNumCoverCells(mC), worstIndexedLevel(wL), bestIndexedLevel(bL) {
    TRI_ASSERT(mC > 0 && wL > 0 && bL > 0 && wL < bL);
  }

  /// @brief read the options from a vpack slice
  void fromVelocyPack(velocypack::Slice slice);

  /// @brief add the options to an opened vpack builder
  void toVelocyPack(velocypack::Builder& builder) const;

  S2RegionCoverer::Options regionCovererOpts() const;

  static constexpr int kMaxNumCoverCellsDefault = 8;
  // Should indicate the max number of cells generated by the S2RegionCoverer
  // is treated as a soft limit, only other params are fixed
  int maxNumCoverCells;
  // Least detailed level used in coverings. Value between [0, 30]
  int worstIndexedLevel;
  // Most detailed level used. Value between [0, 30]
  int bestIndexedLevel;
};

struct QueryParams {
  QueryParams() noexcept
      : origin(S2LatLng::Invalid()),
        cover(queryMaxCoverCells, queryWorstLevel, queryBestLevel) {}

  /// This query only needs to support points no polygons etc
  // bool onlyPoints = false;

  // ============== Near Query Params =============

  /// @brief Min distance from centroid a result has to be
  double minDistance = 0.0;
  /// @brief is minimum an exclusive bound
  bool minInclusive = false;

  /// entire earth (halfaround in each direction),
  /// may not be larger than half earth circumference or larger
  /// than the bounding cap of the filter region (see _filter)
  double maxDistance = kMaxDistanceBetweenPoints;
  bool maxInclusive = false;

  /// Some condition on min and max distances are given, this starts out
  /// as false and must be set whenever the values minDistance, minInclusive
  /// maxDistance and maxInclusive are intended to take effect.
  bool distanceRestricted = false;
  /// @brief results need to be sorted by distance to centroid
  bool sorted = false;
  /// @brief Default order is from closest to farthest
  bool ascending = true;

  /// @brief Centroid from which to sort by distance
  S2LatLng origin;

  // =================== Hints ===================

  /// @brief Index only contains points; no need to consider larger polygons
  bool pointsOnly = false;
  /// @brief If non-zero, we will use a LIMIT clause later with this value
  size_t limit = 0;

  // ============= Filtered Params ===============

  FilterType filterType = FilterType::NONE;
  ShapeContainer filterShape;

  // parameters to calculate the cover for index
  // lookup intervals
  RegionCoverParams cover;

 public:
  /// minimum distance
  double minDistanceRad() const noexcept;

  /// depending on @{filter} and @{region} uses maxDistance or
  /// maxDistance / kEarthRadius or a bounding circle around
  /// the area in region
  double maxDistanceRad() const noexcept;

  /// some defaults for queries
  static constexpr int queryMaxCoverCells = 20;
  static constexpr int queryWorstLevel = 4;
  static constexpr int queryBestLevel = 23;  // about 1m

  std::string toString() const;
};

}  // namespace geo
}  // namespace arangodb
