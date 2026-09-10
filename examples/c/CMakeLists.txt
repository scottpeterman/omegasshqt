# examples/c/CMakeLists.txt
foreach(sample shell_smoke policy_suite knownhosts_probe vault_smoke credential_probe
               transport_probe)
    add_executable(${sample} ${sample}.cpp)
    target_link_libraries(${sample} PRIVATE omegassh::c)
endforeach()