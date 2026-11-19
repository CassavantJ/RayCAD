// The "chamfer" feature type (fillet_feature.h): the blend code of the fillet with an equal-distance chamfer.
#include "fillet_feature.h"

#include "feature_props.h"

#include <string>

namespace ray::doc::features {
namespace {

const std::string& chamferSchema()
{
    static const std::string text = objectSchema(
        R"("edges","distance")", R"("edges":{"type":"array","minItems":1,"items":)" + topoRefSchema() +
                                     R"(,"description":"The edges to bevel, all on one body"},"distance":)" +
                                     lengthSchema("Chamfer distance, equal on both faces"));
    return text;
}

} // namespace

std::unique_ptr<IFeatureType> makeChamferFeatureType()
{
    return makeBlendFeatureType(BlendKind{kChamferType, "Chamfer", "distance", chamferSchema(), false});
}

std::string_view chamferPropsSchema()
{
    return chamferSchema();
}

} // namespace ray::doc::features
