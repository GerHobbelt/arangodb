////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "MetricsDefinitions.h"

namespace arangodb {
namespace metrics {

Metric::Metric(std::string name, 
               Helptext help,
               Description description, 
               Threshold threshold,
               Unit unit, 
               Type type,
               Category category, 
               Complexity complexity,
               std::underlying_type<ExposedBy>::type exposedBy)
    : name(std::move(name)),
      help(std::move(help)),
      description(std::move(description)),
      threshold(std::move(threshold)),
      unit(unit),
      type(type),
      category(category),
      complexity(complexity),
      exposedBy(exposedBy) {}

Metric const testMetric1(
    "arangodb_metric_blabla1",
    Helptext{"this is the help text, normally just a single line"},
    Description{R""""(
this is a long, long long, 
even multiline description text.
it can span as many lines as required.
doesn't really matter.
    )""""},
    Threshold{R""""(
this is some description of thresholds.
can be on multi-lines, or not.
)""""},

    Unit::Number,
    Type::Gauge,
    Category::Agency,
    Complexity::Simple,
    exposedBy(ExposedBy::Single, ExposedBy::Coordinator)
  );

Metric const testMetric2(
    "arangodb_metric_blabla2",
    Helptext{"this is the other help text"},
    Description{R""""(
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
)""""
R""""(
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, this is a long, long long, 
)""""
    },
    Threshold{R""""(
this is some description of thresholds.
can be on multi-lines, or not.
)""""},

    Unit::Number,
    Type::Gauge,
    Category::Agency,
    Complexity::Simple,
    exposedBy(ExposedBy::Single, ExposedBy::Coordinator)
  );


}  // namespace metrics
}  // namespace arangodb
