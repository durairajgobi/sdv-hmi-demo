# Mixed-criticality cockpit: hypervisor partition manager, inter-VM shared
# memory and the IVI guest. The cluster partition is the existing HMI.
add_subdirectory(ivshm)
add_subdirectory(hypervisor)
if(SDV_BUILD_HMI)
    add_subdirectory(ivi-guest)
endif()
