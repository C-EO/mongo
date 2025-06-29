// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/view/view_registry.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<ViewRegistry> ViewRegistryFactory::Create()
{
  std::unique_ptr<ViewRegistry> view_registry(new ViewRegistry());
  return view_registry;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
