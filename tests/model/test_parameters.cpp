// Clip parameters: the stable parameter table and ParamValue.

#include <wabcut/model/Parameters.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;

TEST_CASE("Parameter keys are stable", "[model][parameters]") {
    // These strings are part of the .wabcut format: changing one breaks every saved project.
    std::vector<std::string_view> keys;
    for (const ParameterInfo& p : allParameters()) {
        keys.push_back(p.key);
    }
    CHECK(keys == std::vector<std::string_view>{"motion.position.x", "motion.position.y", "motion.scale",
                                                "motion.rotation", "motion.anchor.x", "motion.anchor.y",
                                                "opacity.opacity", "volume.level", "panner.balance"});
}

TEST_CASE("The parameter table is consistent", "[model][parameters]") {
    REQUIRE(allParameters().size() == kParamCount);
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < allParameters().size(); ++i) {
        const ParameterInfo& p = allParameters()[i];
        INFO(p.key);
        CHECK(static_cast<std::size_t>(p.id) == i);
        CHECK(seen.insert(p.key).second);
        CHECK(parameterByKey(p.key) == p.id);
        CHECK(&parameterInfo(p.id) == &p);
        CHECK(p.minValue <= p.defaultValue);
        CHECK(p.defaultValue <= p.maxValue);
        CHECK(std::isfinite(p.minValue));
        CHECK(std::isfinite(p.maxValue));
        CHECK_FALSE(p.displayName.empty());
    }
    CHECK_FALSE(parameterByKey("motion.skew"));
    CHECK(parameterInfo(ParamId::Opacity).unit == ParamUnit::Fraction);
    CHECK(parameterInfo(ParamId::VolumeDb).role == PropertyRole::Audio);
    CHECK(parameterInfo(ParamId::AnchorY).role == PropertyRole::Motion);
}

TEST_CASE("ClipProperties start at the defaults and check their ranges", "[model][parameters]") {
    ClipProperties p;
    for (const ParameterInfo& info : allParameters()) {
        CHECK(p.isDefault(info.id));
        CHECK(p.value(info.id) == info.defaultValue);
    }
    CHECK(p.value(ParamId::Scale) == 1.0);
    CHECK(p.value(ParamId::Opacity) == 1.0);
    CHECK_FALSE(p.firstInvalid());

    p.set(ParamId::Opacity, 0.25);
    CHECK_FALSE(p.isDefault(ParamId::Opacity));
    CHECK(p[ParamId::Opacity].isConstant());
    CHECK(p[ParamId::Opacity].valueAt(Ticks{123}) == 0.25); // constants do not vary over time
    CHECK(p.differences(ClipProperties{}) == PropertyRoles(PropertyRole::Opacity));

    p.set(ParamId::PositionX, 10.0);
    p.set(ParamId::Pan, -0.5);
    CHECK(p.differences(ClipProperties{}) == (PropertyRole::Opacity | PropertyRole::Motion | PropertyRole::Audio));

    p.set(ParamId::Pan, 1.5);
    CHECK(p.firstInvalid() == ParamId::Pan);
    p.set(ParamId::Pan, 0.0);
    p.set(ParamId::Scale, std::numeric_limits<double>::infinity());
    CHECK(p.firstInvalid() == ParamId::Scale);
}
