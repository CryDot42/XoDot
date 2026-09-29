def can_build(env, platform):
    env.module_add_dependencies("landscape", ["streaming"])
    return not env["disable_3d"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "Landscape3D",
        "LandscapeBrush",
        "LandscapeData",
        "LandscapeFoliage3D",
        "LandscapeFoliageData",
        "LandscapeFoliageType",
        "LandscapeLayer",
        "LandscapeRoadMaterial",
        "LandscapeSpline3D",
        "LandscapeSplineMaterial",
        "LandscapeWaterMaterial",
    ]


def get_doc_path():
    return "doc_classes"
