add_library(lbug_function_aggregate
        OBJECT
        count.cpp
        count_star.cpp
        collect.cpp
        min_max.cpp
        percentile_disc.cpp
        sum.cpp
        avg.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_function_aggregate>
        PARENT_SCOPE)
