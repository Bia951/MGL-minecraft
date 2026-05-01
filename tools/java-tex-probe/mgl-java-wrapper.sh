#!/bin/zsh
AGENT='/Users/fterward/MGL-minecraft/tools/java-tex-probe/mgl-tex-probe-agent.jar'
PROBE_OPTS="-javaagent:${AGENT} -Dmgl.texprobe.width=512 -Dmgl.texprobe.height=512 -Dmgl.texprobe.max=40"
if [[ -n "${JAVA_TOOL_OPTIONS:-}" ]]; then
  export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS} ${PROBE_OPTS}"
else
  export JAVA_TOOL_OPTIONS="${PROBE_OPTS}"
fi
echo "MGLJ WRAPPER active JAVA_TOOL_OPTIONS=${JAVA_TOOL_OPTIONS}" >&2
exec "$@"
