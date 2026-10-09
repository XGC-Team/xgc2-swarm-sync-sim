#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

INSTALL_ROOT=""
OUTPUT_DIR=""
ROS_DISTRO="${ROS_DISTRO:-noetic}"

product_version() {
  awk -F': *' '/^version:[[:space:]]*/ {print $2; exit}' "${REPO_ROOT}/.xgc2/product.yml"
}

VERSION="${PACKAGE_VERSION:-$(product_version)}"
if [[ -z "${VERSION}" ]]; then
  echo "package version is missing; set PACKAGE_VERSION or .xgc2/product.yml version" >&2
  exit 1
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install-root)
      INSTALL_ROOT="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "${INSTALL_ROOT}" || -z "${OUTPUT_DIR}" ]]; then
  echo "--install-root and --output-dir are required" >&2
  exit 1
fi

ARCH="$(dpkg --print-architecture)"
PREFIX="/opt/ros/${ROS_DISTRO}"
PREFIX_ROOT="${INSTALL_ROOT}${PREFIX}"
BUILD_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

mkdir -p "${OUTPUT_DIR}"
rm -f "${OUTPUT_DIR}"/*.deb

copy_path() {
  local src="$1"
  local dst_root="$2"
  if [[ -e "${src}" ]]; then
    mkdir -p "${dst_root}$(dirname "${src#${INSTALL_ROOT}}")"
    cp -a "${src}" "${dst_root}${src#${INSTALL_ROOT}}"
  fi
}

copy_ros_package_paths() {
  local ros_pkg="$1"
  local dst_root="$2"

  copy_path "${PREFIX_ROOT}/share/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/lib/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/include/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/lib/python3/dist-packages/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/share/common-lisp/ros/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/share/gennodejs/ros/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/share/geneus/ros/${ros_pkg}" "${dst_root}"
  copy_path "${PREFIX_ROOT}/share/roseus/ros/${ros_pkg}" "${dst_root}"
}

copy_libs() {
  local dst_root="$1"
  shift
  local lib
  for lib in "$@"; do
    copy_path "${PREFIX_ROOT}/lib/${lib}.so" "${dst_root}"
  done
}

write_control() {
  local pkg_root="$1"
  local package="$2"
  local depends="$3"
  local description="$4"

  mkdir -p "${pkg_root}/DEBIAN" "${pkg_root}/usr/share/doc/${package}"
  cat > "${pkg_root}/DEBIAN/control" <<EOF
Package: ${package}
Version: ${VERSION}
Section: misc
Priority: optional
Architecture: ${ARCH}
Maintainer: XGC2 <apt@example.com>
Depends: ${depends}
Description: ${description}
EOF
  printf 'swarm-sync-sim package\n' > "${pkg_root}/usr/share/doc/${package}/README"
  chmod 0755 "${pkg_root}/DEBIAN"
}

build_deb() {
  local package="$1"
  local ros_pkg="$2"
  local depends="$3"
  local description="$4"
  shift 4

  local pkg_root="${BUILD_DIR}/${package}"
  rm -rf "${pkg_root}"
  mkdir -p "${pkg_root}"

  if [[ -n "${ros_pkg}" ]]; then
    copy_ros_package_paths "${ros_pkg}" "${pkg_root}"
  fi
  if [[ "$#" -gt 0 ]]; then
    copy_libs "${pkg_root}" "$@"
  fi

  write_control "${pkg_root}" "${package}" "${depends}" "${description}"
  fakeroot dpkg-deb --build "${pkg_root}" "${OUTPUT_DIR}/${package}_${VERSION}_${ARCH}.deb" >/dev/null
}

base_depends="ros-noetic-roscpp, ros-noetic-rospy, ros-noetic-std-msgs"
sim_env_pkg="ros-noetic-sss-sim-env"
px4_rotor_pkg="ros-noetic-sss-px4-rotor-sim"
tello_pkg="ros-noetic-sss-tello-sim"
ugv_pkg="ros-noetic-sss-ugv-sim"
fw_plane_pkg="ros-noetic-sss-fw-plane-sim"

build_deb \
  "${sim_env_pkg}" \
  "sss_sim_env" \
  "${base_depends}, libxgc2-xrpc1 (>= 0.1.0), libjsoncpp1 (>= 1.7.4), ros-noetic-message-runtime, ros-noetic-nodelet, ros-noetic-pluginlib, ros-noetic-rosgraph-msgs, python3-pyqt5" \
  "XGC2 Swarm Sync Sim shared simulation clock and utilities" \
  libsim_clock libClockUpdater libsss_timer libsss_sleep

build_deb \
  "${px4_rotor_pkg}" \
  "px4_rotor_sim" \
  "${sim_env_pkg} (= ${VERSION}), ${base_depends}, ros-noetic-mavros, ros-noetic-mavros-msgs, ros-noetic-mavros-extras, ros-noetic-eigen-conversions, ros-noetic-geometry-msgs, ros-noetic-nav-msgs, ros-noetic-sensor-msgs, ros-noetic-tf-conversions, ros-noetic-tf2-geometry-msgs, ros-noetic-tf2-ros, ros-noetic-nodelet, ros-noetic-pluginlib, ros-noetic-visualization-msgs, ros-noetic-robot-state-publisher, ros-noetic-rviz, libboost-dev, libeigen3-dev" \
  "XGC2 Swarm Sync Sim PX4 rotor simulation package" \
  libpx4_rotor_dynamics libpx4_lib libmc_pos_control libmc_att_control libcommander libpx4_mavlink libpx4_sitl libMavrosSim libpx4_rotor_visualizer libmavros_px4_quadrotor_sim_nodelet

build_deb \
  "${tello_pkg}" \
  "tello_sim" \
  "${sim_env_pkg} (= ${VERSION}), ${base_depends}, ros-noetic-sensor-msgs, ros-noetic-geometry-msgs, ros-noetic-nav-msgs, ros-noetic-nodelet, ros-noetic-pluginlib, ros-noetic-tf2-ros, ros-noetic-visualization-msgs, ros-noetic-robot-state-publisher, ros-noetic-rviz, libboost-dev, libeigen3-dev" \
  "XGC2 Swarm Sync Sim Tello simulation package" \
  libtello_dynamics libtello_driver_sim libtello_quadrotor_sim_nodelet

build_deb \
  "${ugv_pkg}" \
  "ugv_sim" \
  "${sim_env_pkg} (= ${VERSION}), ${base_depends}, ros-noetic-sensor-msgs, ros-noetic-geometry-msgs, ros-noetic-nav-msgs, ros-noetic-nodelet, ros-noetic-pluginlib, ros-noetic-tf2-ros, ros-noetic-visualization-msgs, ros-noetic-robot-state-publisher, ros-noetic-rviz, ros-noetic-xacro, libboost-dev, libeigen3-dev" \
  "XGC2 Swarm Sync Sim UGV simulation package" \
  libugv_dynamics libwheeltec_driver_sim libwheeltec_ugv_sim_nodelet

meta_depends="${sim_env_pkg} (= ${VERSION}), ${px4_rotor_pkg} (= ${VERSION}), ${tello_pkg} (= ${VERSION}), ${ugv_pkg} (= ${VERSION})"

if [[ "${ARCH}" == "amd64" ]]; then
  build_deb \
    "${fw_plane_pkg}" \
    "fw_plane_sim" \
    "${sim_env_pkg} (= ${VERSION}), ${base_depends}, ros-noetic-mavros, ros-noetic-mavros-msgs, ros-noetic-mavros-extras, ros-noetic-eigen-conversions, ros-noetic-geometry-msgs, ros-noetic-nav-msgs, ros-noetic-sensor-msgs, ros-noetic-tf-conversions, ros-noetic-tf2-geometry-msgs, ros-noetic-tf2-ros, ros-noetic-nodelet, ros-noetic-pluginlib, ros-noetic-visualization-msgs, ros-noetic-robot-state-publisher, ros-noetic-rviz, libboost-dev, libeigen3-dev" \
    "XGC2 Swarm Sync Sim fixed-wing plane simulation package" \
    libpx4_lib_fw libfw_plane_visualizer libfw_sim_nodelet libBHDynamic
  meta_depends="${meta_depends}, ${fw_plane_pkg} (= ${VERSION})"
fi

build_deb \
  "ros-noetic-swarm-sync-sim" \
  "" \
  "${meta_depends}" \
  "XGC2 Swarm Sync Sim aggregate package"

find "${OUTPUT_DIR}" -maxdepth 1 -type f -name '*.deb' -print | sort
