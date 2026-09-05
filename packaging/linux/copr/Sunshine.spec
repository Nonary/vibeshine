%global build_timestamp %(date +"%Y%m%d")

# use sed to replace these values
%global build_version 0
%global branch 0
%global commit 0

# Keep the application-facing version strict SemVer while translating only
# the RPM package version into RPM ordering syntax. Prereleases sort below the
# final release; stable.N respins sort above it.
%global build_semver %{lua:local v=rpm.expand("%{build_version}"); if v:sub(1,1)=="v" then v=v:sub(2) end; print(v)}
%global rpm_version %{lua:local v=rpm.expand("%{build_semver}"); local p=v:find("-",1,true); if p then if v:sub(p+1,p+6)=="stable" then v=v:sub(1,p-1).."+stable"..v:sub(p+7) else v=v:sub(1,p-1).."~"..v:sub(p+1) end end; print(v)}

%undefine _hardened_build

# Define _metainfodir for OpenSUSE if not already defined
%if 0%{?suse_version}
%if !0%{?_metainfodir:1}
%global _metainfodir %{_datadir}/metainfo
%endif
%endif

Name: vibeshine
Version: %{rpm_version}
Release: 1%{?dist}
Summary: Self-hosted game stream host for Moonlight.
License: GPLv3-only
URL: https://github.com/Nonary/vibeshine
Source0: tarball.tar.gz

# Common BuildRequires
BuildRequires: cmake >= 3.25.0
BuildRequires: desktop-file-utils
BuildRequires: git
BuildRequires: libcap-devel
BuildRequires: libcurl-devel
BuildRequires: libdrm-devel
BuildRequires: libevdev-devel
BuildRequires: libnotify-devel
BuildRequires: libva-devel
BuildRequires: libX11-devel
BuildRequires: libxcb-devel
BuildRequires: libXcursor-devel
BuildRequires: libXfixes-devel
BuildRequires: libXi-devel
BuildRequires: libXinerama-devel
BuildRequires: libXrandr-devel
BuildRequires: libXtst-devel
BuildRequires: openssl-devel
BuildRequires: pipewire-devel
BuildRequires: rpm-build
BuildRequires: systemd-rpm-macros
BuildRequires: wget
BuildRequires: which

%if 0%{?fedora}
# Fedora-specific BuildRequires
BuildRequires: appstream
# BuildRequires: boost-devel >= 1.86.0
BuildRequires: glslc
BuildRequires: libappstream-glib
BuildRequires: vulkan-loader-devel
BuildRequires: libayatana-appindicator3-devel
BuildRequires: libgudev
BuildRequires: mesa-libGL-devel
BuildRequires: mesa-libgbm-devel
BuildRequires: miniupnpc-devel
BuildRequires: numactl-devel
BuildRequires: opus-devel
BuildRequires: pulseaudio-libs-devel
BuildRequires: python3-jinja2
BuildRequires: python3-setuptools
BuildRequires: systemd-udev
%{?sysusers_requires_compat}
%endif

%if 0%{?suse_version}
# OpenSUSE-specific BuildRequires
BuildRequires: AppStream
BuildRequires: appstream-glib
BuildRequires: libappindicator3-devel
BuildRequires: libgudev-1_0-devel
BuildRequires: Mesa-libGL-devel
BuildRequires: libgbm-devel
BuildRequires: libminiupnpc-devel
BuildRequires: libnuma-devel
BuildRequires: libopus-devel
BuildRequires: libpulse-devel
BuildRequires: python311
BuildRequires: python311-Jinja2
BuildRequires: python311-setuptools
%if !0%{?sle_version}
BuildRequires: shaderc
%endif
BuildRequires: udev
%if !0%{?sle_version}
BuildRequires: vulkan-devel
%endif
%endif

# Conditional BuildRequires for cuda-gcc based on distribution version
%if 0%{?fedora}
%if 0%{?fedora} <= 41
BuildRequires: gcc13
BuildRequires: gcc13-c++
%global gcc_version 13
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 42 && 0%{?fedora} <= 43
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 44
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%endif
%endif

%if 0%{?suse_version}
%if 0%{?suse_version} <= 1699
# OpenSUSE Leap 15.x
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%else
# OpenSUSE Tumbleweed
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%endif
%endif

%global cuda_dir %{_builddir}/cuda

# Common runtime requirements
Requires: miniupnpc >= 2.2.4
Requires: kmod
Requires: iproute
Requires: jq
Requires: /usr/bin/pactl
Requires: /usr/bin/parec
Requires: /usr/bin/wayland-info
Requires: /usr/bin/xdpyinfo
Requires: socat
Requires: util-linux
Recommends: dkms
Recommends: gcc
Recommends: kernel-devel
Recommends: make

%if 0%{?fedora}
# Fedora runtime requirements
Requires: libayatana-appindicator3 >= 0.5.3
Requires: libcap >= 2.22
Requires: libcurl >= 7.0
Requires: libdrm > 2.4.97
Requires: libevdev >= 1.5.6
Requires: libkscreen
Requires: libopusenc >= 0.2.1
Requires: libva >= 2.14.0
Requires: libwayland-client >= 1.20.0
Requires: libX11 >= 1.7.3.1
Requires: numactl-libs >= 2.0.14
Requires: openssl >= 3.0.2
Requires: pulseaudio-libs >= 10.0
Requires: vulkan-loader
%endif

%if 0%{?suse_version}
# OpenSUSE runtime requirements
Requires: libappindicator3-1
Requires: libcap2
Requires: libcurl4
Requires: libdrm2
Requires: libevdev2
# The binary moved between openSUSE KScreen package generations; use the RPM
# file capability so zypper selects the provider for the active release.
Requires: /usr/bin/kscreen-doctor
Requires: libopusenc0
Requires: libva2
Requires: libwayland-client0
Requires: libX11-6
Requires: libnuma1
Requires: libopenssl3
Requires: libpulse0
%if !0%{?sle_version}
Requires: libvulkan1
%endif
%endif

%description
Self-hosted game stream host for Moonlight.

%prep
# extract tarball to current directory
mkdir -p %{_builddir}/Sunshine
tar -xzf %{SOURCE0} -C %{_builddir}/Sunshine

# list directory
ls -a %{_builddir}/Sunshine

%build
# exit on error
set -e

# Detect the architecture and Fedora version
architecture=$(uname -m)

cuda_supported_architectures=("x86_64" "aarch64")

# prepare CMAKE args
cmake_args=(
  "-B=%{_builddir}/Sunshine/build"
  "-G=Unix Makefiles"
  "-S=."
  "-DBUILD_DOCS=OFF"
  "-DBUILD_TESTS=ON"
  "-DBUILD_WERROR=ON"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_PREFIX=%{_prefix}"
  "-DSUNSHINE_ASSETS_DIR=%{_datadir}/vibeshine"
  "-DSUNSHINE_EXECUTABLE_PATH=%{_bindir}/vibeshine"
  "-DSUNSHINE_ENABLE_DRM=ON"
  "-DSUNSHINE_ENABLE_KWIN=ON"
  "-DSUNSHINE_ENABLE_PORTAL=ON"
  "-DSUNSHINE_ENABLE_WAYLAND=ON"
  "-DSUNSHINE_ENABLE_X11=ON"
  "-DSUNSHINE_PUBLISHER_NAME=Nonary"
  "-DSUNSHINE_PUBLISHER_WEBSITE=https://github.com/Nonary/vibeshine"
  "-DSUNSHINE_PUBLISHER_ISSUE_URL=https://github.com/Nonary/vibeshine/issues"
)

export CC=gcc-%{gcc_version}
export CXX=g++-%{gcc_version}

function install_cuda() {
  # check if we need to install cuda
  if [ -f "%{cuda_dir}/bin/nvcc" ]; then
    echo "cuda already installed"
    return
  fi

  local cuda_prefix="https://developer.download.nvidia.com/compute/cuda/"
  local cuda_suffix=""
  if [ "$architecture" == "aarch64" ]; then
    local cuda_suffix="_sbsa"
  fi

  local url="${cuda_prefix}%{cuda_version}/local_installers/cuda_%{cuda_version}_%{cuda_build}_linux${cuda_suffix}.run"
  echo "cuda url: ${url}"
  wget \
    "$url" \
    --progress=bar:force:noscroll \
    --retry-connrefused \
    --tries=3 \
    -q -O "%{_builddir}/cuda.run"
  chmod a+x "%{_builddir}/cuda.run"
  "%{_builddir}/cuda.run" \
    --no-drm \
    --no-man-page \
    --no-opengl-libs \
    --override \
    --silent \
    --toolkit \
    --toolkitpath="%{cuda_dir}"
  rm "%{_builddir}/cuda.run"

  # we need to patch math_functions.h depending on the CUDA major version
  # see https://forums.developer.nvidia.com/t/error-exception-specification-is-incompatible-for-cospi-sinpi-cospif-sinpif-with-glibc-2-41/323591/3
  local cuda_major
  cuda_major=$(echo "%{cuda_version}" | cut -d. -f1)
  local patch_file=""
  if [ "${cuda_major}" -eq 12 ]; then
    # CUDA 12.x: the extern declarations lack noexcept(true); add it to match glibc 2.41.
    patch_file="cuda-12-math_functions.patch"
  elif [ "${cuda_major}" -eq 13 ]; then
    # CUDA 13.x: the extern declarations already have noexcept(true), but the __func__()
    # macro invocations at the bottom still lack it, causing a redeclaration conflict.
    patch_file="cuda-13-math_functions.patch"
  else
    echo "Warning: no math_functions.h patch available for CUDA ${cuda_major}.x, skipping."
  fi

  if [ -n "${patch_file}" ]; then
    echo "Applying CUDA patch: ${patch_file}"
    patch -p2 \
      --backup \
      --directory="%{cuda_dir}" \
      --verbose \
      < "%{_builddir}/Sunshine/packaging/linux/patches/${architecture}/${patch_file}"
  fi
}

if [ -n "%{cuda_version}" ] && [[ " ${cuda_supported_architectures[@]} " =~ " ${architecture} " ]]; then
  install_cuda
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=ON" "-DSUNSHINE_REQUIRE_CUDA_PASCAL=ON")
  cmake_args+=("-DCMAKE_CUDA_COMPILER:PATH=%{cuda_dir}/bin/nvcc")
  cmake_args+=("-DCMAKE_CUDA_HOST_COMPILER=gcc-%{gcc_version}")
else
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=OFF")
fi

# setup the version
export BRANCH=%{branch}
export BUILD_VERSION=%{build_semver}
export COMMIT=%{commit}

# Disable Vulkan on openSUSE Leap (shaderc/glslang not in official repos)
%if 0%{?sle_version}
cmake_args+=("-DSUNSHINE_ENABLE_VULKAN=OFF")
%endif

# cmake
cd %{_builddir}/Sunshine
echo "cmake args:"
echo "${cmake_args[@]}"
cmake "${cmake_args[@]}"
make -j$(nproc) -C "%{_builddir}/Sunshine/build"

%check
# validate the metainfo file
appstreamcli validate %{buildroot}%{_metainfodir}/*.metainfo.xml
appstream-util validate %{buildroot}%{_metainfodir}/*.metainfo.xml
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop

%install
cd %{_builddir}/Sunshine/build
%make_install

%pre
vibeshine_controller=%{_prefix}/libexec/vibeshine/vibeshine-session-controller
vibeshine_legacy_host=%{_prefix}/libexec/vibeshine/vibeshine-machine-host
vibeshine_legacy_handoff=%{_prefix}/libexec/vibeshine/vibeshine-session-handoff
vibeshine_runtime_root=/run/vibeshine
vibeshine_broker_socket=/run/vibeshine/session-broker.sock
vibeshine_control_socket=/run/vibeshine/vkms-control.sock
vibeshine_host_upgrade_dropin_dir=/run/systemd/system/vibeshine.service.d
vibeshine_host_upgrade_dropin=$vibeshine_host_upgrade_dropin_dir/90-vibeshine-safe-upgrade.conf
vibeshine_controller_was_frozen=0
vibeshine_upgrade_kill_mode=
vibeshine_legacy_handoff_directory=/run/vibeshine/session-handoffs
vibeshine_legacy_restore_directory=/run/vibeshine/session-restores
vibeshine_legacy_transition_lock=/run/vibeshine/session-handoff.lock
vibeshine_legacy_prelogin_marker=/run/vibeshine/prelogin-handoff-complete
vibeshine_cgroup_is_quiescent() {
  vibeshine_control_group=$1
  [ -n "$vibeshine_control_group" ] || return 0
  case "$vibeshine_control_group" in /*) ;; *) return 1 ;; esac
  case "$vibeshine_control_group" in */../* | */..) return 1 ;; esac
  vibeshine_cgroup_path=/sys/fs/cgroup$vibeshine_control_group
  if [ ! -e "$vibeshine_cgroup_path" ] && [ ! -L "$vibeshine_cgroup_path" ]; then return 0; fi
  [ -d "$vibeshine_cgroup_path" ] && [ ! -L "$vibeshine_cgroup_path" ] && \
    [ -f "$vibeshine_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibeshine_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibeshine_cgroup_path/cgroup.events"
}
vibeshine_unit_is_quiescent() {
  vibeshine_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  [ "$(printf '%%s\n' "$vibeshine_properties" | wc -l | tr -d ' ')" = 5 ] || return 1
  for vibeshine_property in LoadState ActiveState SubState MainPID ControlGroup; do
    vibeshine_property_count=$(printf '%%s\n' "$vibeshine_properties" | \
      grep -c "^$vibeshine_property=" || true)
    [ "$vibeshine_property_count" = 1 ] || return 1
  done
  vibeshine_load=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^LoadState=//p')
  vibeshine_state=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^ActiveState=//p')
  vibeshine_substate=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^SubState=//p')
  vibeshine_pid=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^MainPID=//p')
  vibeshine_control_group=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibeshine_load:$vibeshine_state:$vibeshine_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibeshine_pid" = 0 ] && vibeshine_cgroup_is_quiescent "$vibeshine_control_group"
}
vibeshine_unit_is_disabled() {
  vibeshine_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  case "$vibeshine_enabled" in
    disabled | masked | masked-runtime | static | indirect | generated | transient | linked | linked-runtime | not-found) return 0 ;;
    *) return 1 ;;
  esac
}
vibeshine_unit_is_masked() {
  vibeshine_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  vibeshine_load=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_enabled" in
    masked | masked-runtime) [ "$vibeshine_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_restore_template_is_masked() {
  vibeshine_restore_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    'vibeshine-session-restore@.service' 2>/dev/null || true)
  vibeshine_restore_load=$(timeout --signal=KILL 5 systemctl show \
    'vibeshine-session-restore@.service' --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_restore_mask" in
    masked | masked-runtime) [ "$vibeshine_restore_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_host_unit_is_masked() {
  vibeshine_host_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine.service 2>/dev/null || true)
  vibeshine_host_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine.service --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_host_mask" in
    masked | masked-runtime) [ "$vibeshine_host_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_broker_socket_is_masked() {
  vibeshine_socket_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine-session-exec.socket 2>/dev/null || true)
  vibeshine_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-session-exec.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_socket_mask" in
    masked | masked-runtime) [ "$vibeshine_socket_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_control_socket_is_masked() {
  vibeshine_socket_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine-vkms-control.socket 2>/dev/null || true)
  vibeshine_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-vkms-control.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_socket_mask" in
    masked | masked-runtime) [ "$vibeshine_socket_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_restore_unit_is_safe() {
  vibeshine_restore_unit=$1
  case "$vibeshine_restore_unit" in vibeshine-session-restore@*.service) ;; *) return 1 ;; esac
  vibeshine_restore_instance=${vibeshine_restore_unit#vibeshine-session-restore@}
  vibeshine_restore_instance=${vibeshine_restore_instance%.service}
  case "$vibeshine_restore_instance" in [1-9]*) ;; *) return 1 ;; esac
  case "$vibeshine_restore_instance" in *[!0-9]*) return 1 ;; esac
}
vibeshine_broker_unit_is_safe() {
  vibeshine_broker_unit=$1
  case "$vibeshine_broker_unit" in vibeshine-session-exec@*.service) ;; *) return 1 ;; esac
  vibeshine_broker_instance=${vibeshine_broker_unit#vibeshine-session-exec@}
  vibeshine_broker_instance=${vibeshine_broker_instance%.service}
  [ -n "$vibeshine_broker_instance" ] || return 1
  case "$vibeshine_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibeshine_control_unit_is_safe() {
  vibeshine_control_unit=$1
  case "$vibeshine_control_unit" in vibeshine-vkms-control@*.service) ;; *) return 1 ;; esac
  vibeshine_control_instance=${vibeshine_control_unit#vibeshine-vkms-control@}
  vibeshine_control_instance=${vibeshine_control_instance%.service}
  [ -n "$vibeshine_control_instance" ] || return 1
  case "$vibeshine_control_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibeshine_stop_exact_unit() {
  # Never bypass a service's ordered resource teardown.  Killing systemctl
  # only abandons the client while its manager job continues; killing the unit
  # cgroup can strand live GPU imports and is therefore forbidden here.
  # Controller cleanup can legitimately spend more than 30 seconds draining
  # the host and GPU bindings; keep waiting for the manager's ordered stop.
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}
vibeshine_bounded_unit_list() (
  vibeshine_unit_pattern=$1
  vibeshine_unit_list=$(mktemp /run/vibeshine-unit-list.XXXXXX) || exit 1
  trap 'rm -f -- "$vibeshine_unit_list"' 0
  trap 'exit 1' HUP INT TERM
  chmod 0600 "$vibeshine_unit_list" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units --all --plain \
      --no-legend --no-pager --full "$vibeshine_unit_pattern" \
      >"$vibeshine_unit_list" 2>/dev/null
  ) || return 1
  vibeshine_unit_list_size=$(stat -c '%%s' -- "$vibeshine_unit_list") || return 1
  case "$vibeshine_unit_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibeshine_unit_list_size" -le 65536 ] || return 1
  cat -- "$vibeshine_unit_list"
)
vibeshine_control_instances_are_quiescent() (
  vibeshine_control_attempt=0
  vibeshine_control_clean_passes=0
  while [ "$vibeshine_control_attempt" -lt 100 ]; do
    vibeshine_control_dirty=0
    vibeshine_controls=$(vibeshine_bounded_unit_list \
      'vibeshine-vkms-control@*.service') || return 1
    vibeshine_seen_units='
'
    while IFS= read -r vibeshine_line || [ -n "$vibeshine_line" ]; do
      [ -n "$vibeshine_line" ] || continue
      case "$vibeshine_line" in *"\r"*) return 1 ;; esac
      set -f
      set -- $vibeshine_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibeshine_unit=$1; vibeshine_load=$2
      vibeshine_state=$3; vibeshine_substate=$4
      vibeshine_control_unit_is_safe "$vibeshine_unit" || return 1
      [ "$vibeshine_load" = loaded ] || return 1
      case "$vibeshine_state:$vibeshine_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibeshine_seen_units" in *"
$vibeshine_unit
"*) return 1 ;; esac
      vibeshine_seen_units="$vibeshine_seen_units$vibeshine_unit
"
      vibeshine_unit_is_quiescent "$vibeshine_unit" || vibeshine_control_dirty=1
    done <<EOF
$vibeshine_controls
EOF
    if [ "$vibeshine_control_dirty" -eq 0 ]; then
      vibeshine_control_clean_passes=$((vibeshine_control_clean_passes + 1))
      [ "$vibeshine_control_clean_passes" -lt 5 ] || {
        [ ! -e "$vibeshine_control_socket" ] && [ ! -L "$vibeshine_control_socket" ]
        return
      }
    else
      vibeshine_control_clean_passes=0
    fi
    vibeshine_control_attempt=$((vibeshine_control_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibeshine_stop_brokers() (
  vibeshine_broker_attempt=0
  vibeshine_broker_clean_passes=0
  while [ "$vibeshine_broker_attempt" -lt 20 ]; do
    vibeshine_broker_dirty=0
    vibeshine_brokers=$(vibeshine_bounded_unit_list \
      'vibeshine-session-exec@*.service') || return 1
    vibeshine_seen_units='
'
    while IFS= read -r vibeshine_line || [ -n "$vibeshine_line" ]; do
      [ -n "$vibeshine_line" ] || continue
      case "$vibeshine_line" in *"
"*) return 1 ;; esac
      set -f
      set -- $vibeshine_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibeshine_unit=$1; vibeshine_load=$2
      vibeshine_state=$3; vibeshine_substate=$4
      vibeshine_broker_unit_is_safe "$vibeshine_unit" || return 1
      [ "$vibeshine_load" = loaded ] || return 1
      case "$vibeshine_state:$vibeshine_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibeshine_seen_units" in *"
$vibeshine_unit
"*) return 1 ;; esac
      vibeshine_seen_units="$vibeshine_seen_units$vibeshine_unit
"
      if ! vibeshine_unit_is_quiescent "$vibeshine_unit"; then
        vibeshine_broker_dirty=1
        vibeshine_stop_exact_unit "$vibeshine_unit"
        vibeshine_unit_is_quiescent "$vibeshine_unit" || return 1
      fi
    done <<EOF
$vibeshine_brokers
EOF
    if [ "$vibeshine_broker_dirty" -eq 0 ]; then
      vibeshine_broker_clean_passes=$((vibeshine_broker_clean_passes + 1))
      [ "$vibeshine_broker_clean_passes" -lt 5 ] || return 0
    else
      vibeshine_broker_clean_passes=0
    fi
    vibeshine_broker_attempt=$((vibeshine_broker_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibeshine_stop_restore_instances() (
  vibeshine_restores=$(vibeshine_bounded_unit_list \
    'vibeshine-session-restore@*.service') || return 1
  vibeshine_seen_units='
'
  while IFS= read -r vibeshine_line || [ -n "$vibeshine_line" ]; do
    [ -n "$vibeshine_line" ] || continue
    case "$vibeshine_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibeshine_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibeshine_unit=$1; vibeshine_load=$2
    vibeshine_state=$3; vibeshine_substate=$4
    vibeshine_restore_unit_is_safe "$vibeshine_unit" || return 1
    case "$vibeshine_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibeshine_state:$vibeshine_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibeshine_seen_units" in *"
$vibeshine_unit
"*) return 1 ;; esac
    vibeshine_seen_units="$vibeshine_seen_units$vibeshine_unit
"
    vibeshine_stop_exact_unit "$vibeshine_unit"
    vibeshine_unit_is_quiescent "$vibeshine_unit" || return 1
  done <<EOF
$vibeshine_restores
EOF
)
vibeshine_brokers_are_quiescent() {
  vibeshine_stop_brokers
}
vibeshine_restore_instances_are_quiescent() (
  vibeshine_restores=$(vibeshine_bounded_unit_list \
    'vibeshine-session-restore@*.service') || return 1
  vibeshine_seen_units='
'
  while IFS= read -r vibeshine_line || [ -n "$vibeshine_line" ]; do
    [ -n "$vibeshine_line" ] || continue
    case "$vibeshine_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibeshine_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibeshine_unit=$1; vibeshine_load=$2
    vibeshine_state=$3; vibeshine_substate=$4
    vibeshine_restore_unit_is_safe "$vibeshine_unit" || return 1
    case "$vibeshine_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibeshine_state:$vibeshine_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibeshine_seen_units" in *"
$vibeshine_unit
"*) return 1 ;; esac
    vibeshine_seen_units="$vibeshine_seen_units$vibeshine_unit
"
    vibeshine_unit_is_quiescent "$vibeshine_unit" || return 1
  done <<EOF
$vibeshine_restores
EOF
)
vibeshine_privileged_helper_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = \
    '0:0:755:1:regular file' ]
}
vibeshine_select_upgrade_kill_mode() {
  if [ ! -e "$vibeshine_legacy_host" ] && [ ! -L "$vibeshine_legacy_host" ]; then
    vibeshine_unit_is_quiescent vibeshine.service || return 1
    vibeshine_upgrade_kill_mode=control-group
    return 0
  fi
  vibeshine_privileged_helper_is_safe "$vibeshine_legacy_host" || return 1
  if grep -Fqx '  trap mark_host_shutdown TERM INT HUP' "$vibeshine_legacy_host"; then
    vibeshine_upgrade_kill_mode=control-group
  elif grep -Fqx "  trap 'forward_host_signal TERM' TERM" "$vibeshine_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal INT' INT" "$vibeshine_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal HUP' HUP" "$vibeshine_legacy_host"; then
    vibeshine_upgrade_kill_mode=process
  else
    return 1
  fi
}
vibeshine_prepare_host_upgrade_fence() (
  [ ! -L "$vibeshine_host_upgrade_dropin_dir" ] && \
    [ ! -L "$vibeshine_host_upgrade_dropin" ] || exit 1
  install -d -o root -g root -m 0755 -- "$vibeshine_host_upgrade_dropin_dir" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibeshine_host_upgrade_dropin_dir")" = \
    '0:0:755:directory' ] || exit 1
  vibeshine_upgrade_temporary=$(mktemp /run/vibeshine-host-upgrade.XXXXXX) || exit 1
  trap 'rm -f -- "$vibeshine_upgrade_temporary"' 0
  case "$vibeshine_upgrade_kill_mode" in process | control-group) ;; *) exit 1 ;; esac
  printf '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=%%s\nSendSIGKILL=no\n' \
    "$vibeshine_upgrade_kill_mode" >"$vibeshine_upgrade_temporary" || exit 1
  chmod 0600 -- "$vibeshine_upgrade_temporary" || exit 1
  if [ -e "$vibeshine_host_upgrade_dropin" ]; then
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibeshine_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || exit 1
    cmp -s -- "$vibeshine_upgrade_temporary" "$vibeshine_host_upgrade_dropin" || exit 1
  fi
  install -o root -g root -m 0644 -- "$vibeshine_upgrade_temporary" \
    "$vibeshine_host_upgrade_dropin" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibeshine_host_upgrade_dropin")" = \
    '0:0:644:1:regular file' ] || exit 1
)
vibeshine_activate_host_upgrade_fence() {
  systemctl daemon-reload || exit 1
  vibeshine_host_stop_properties=$(timeout --signal=KILL 5 systemctl show vibeshine.service \
    --property=RefuseManualStart --property=KillMode --property=SendSIGKILL 2>/dev/null) || exit 1
  printf '%%s\n' "$vibeshine_host_stop_properties" | grep -qx 'RefuseManualStart=yes' || exit 1
  printf '%%s\n' "$vibeshine_host_stop_properties" | \
    grep -qx "KillMode=$vibeshine_upgrade_kill_mode" || exit 1
  printf '%%s\n' "$vibeshine_host_stop_properties" | grep -qx 'SendSIGKILL=no' || exit 1
}
vibeshine_host_is_stable_or_quiescent() {
  vibeshine_host_state=$(timeout --signal=KILL 5 systemctl show vibeshine.service \
    --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup --property=Job 2>/dev/null) || return 1
  vibeshine_host_active=$(printf '%%s\n' "$vibeshine_host_state" | sed -n 's/^ActiveState=//p')
  vibeshine_host_substate=$(printf '%%s\n' "$vibeshine_host_state" | sed -n 's/^SubState=//p')
  vibeshine_host_pid=$(printf '%%s\n' "$vibeshine_host_state" | sed -n 's/^MainPID=//p')
  vibeshine_host_cgroup=$(printf '%%s\n' "$vibeshine_host_state" | sed -n 's/^ControlGroup=//p')
  vibeshine_host_job=$(printf '%%s\n' "$vibeshine_host_state" | sed -n 's/^Job=//p')
  [ -z "$vibeshine_host_job" ] || return 1
  if [ "$vibeshine_host_active" = active ] && [ "$vibeshine_host_substate" = running ]; then
    case "$vibeshine_host_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
    case "$vibeshine_host_cgroup" in /*) return 0 ;; *) return 1 ;; esac
  fi
  case "$vibeshine_host_active:$vibeshine_host_substate:$vibeshine_host_pid" in
    inactive:dead:0 | failed:failed:0) vibeshine_cgroup_is_quiescent "$vibeshine_host_cgroup" ;;
    *) return 1 ;;
  esac
}
vibeshine_freeze_controller() {
  if vibeshine_unit_is_quiescent vibeshine-session-controller.service; then
    vibeshine_controller_was_frozen=0
    return 0
  fi
  vibeshine_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  vibeshine_controller_active=$(printf '%%s\n' "$vibeshine_controller_state" | sed -n 's/^ActiveState=//p')
  vibeshine_controller_substate=$(printf '%%s\n' "$vibeshine_controller_state" | sed -n 's/^SubState=//p')
  vibeshine_controller_pid=$(printf '%%s\n' "$vibeshine_controller_state" | sed -n 's/^MainPID=//p')
  vibeshine_controller_cgroup=$(printf '%%s\n' "$vibeshine_controller_state" | sed -n 's/^ControlGroup=//p')
  [ "$vibeshine_controller_active" = active ] && [ "$vibeshine_controller_substate" = running ] || return 1
  case "$vibeshine_controller_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
  case "$vibeshine_controller_cgroup" in /*) ;; *) return 1 ;; esac
  case "$vibeshine_controller_cgroup" in */../* | */..) return 1 ;; esac
  timeout --signal=TERM --kill-after=2 15 systemctl freeze \
    vibeshine-session-controller.service 2>/dev/null || return 1
  vibeshine_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx "MainPID=$vibeshine_controller_pid" && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx "ControlGroup=$vibeshine_controller_cgroup" && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'FreezerState=frozen' || return 1
  vibeshine_controller_freeze_path=/sys/fs/cgroup$vibeshine_controller_cgroup/cgroup.freeze
  vibeshine_controller_events_path=/sys/fs/cgroup$vibeshine_controller_cgroup/cgroup.events
  [ -f "$vibeshine_controller_freeze_path" ] && [ ! -L "$vibeshine_controller_freeze_path" ] && \
    grep -qx '1' "$vibeshine_controller_freeze_path" && \
    [ -f "$vibeshine_controller_events_path" ] && [ ! -L "$vibeshine_controller_events_path" ] && \
    grep -qx 'populated 1' "$vibeshine_controller_events_path" && \
    grep -qx 'frozen 1' "$vibeshine_controller_events_path" || return 1
  vibeshine_controller_was_frozen=1
}
vibeshine_controller_remains_frozen() {
  [ "$vibeshine_controller_was_frozen" -eq 1 ] || return 0
  vibeshine_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx "MainPID=$vibeshine_controller_pid" && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx "ControlGroup=$vibeshine_controller_cgroup" && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'FreezerState=frozen'
}
vibeshine_thaw_controller() {
  [ "$vibeshine_controller_was_frozen" -eq 1 ] || return 0
  timeout --signal=KILL 15 systemctl thaw \
    vibeshine-session-controller.service 2>/dev/null || return 1
  vibeshine_controller_was_frozen=0
  vibeshine_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx "MainPID=$vibeshine_controller_pid" && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx "ControlGroup=$vibeshine_controller_cgroup" && \
    printf '%%s\n' "$vibeshine_controller_state" | grep -qx 'FreezerState=running'
}
vibeshine_run_optional_legacy_command() {
  if [ ! -e "$vibeshine_legacy_host" ] && [ ! -L "$vibeshine_legacy_host" ]; then return 2; fi
  vibeshine_privileged_helper_is_safe "$vibeshine_legacy_host" || return 1
  timeout --signal=KILL 40 "$vibeshine_legacy_host" "$1" >/dev/null 2>&1
  vibeshine_command_status=$?
  [ "$vibeshine_command_status" -eq 0 ] && return 0
  [ "$vibeshine_command_status" -eq 2 ] && return 2
  return 1
}
vibeshine_runtime_root_is_safe_or_absent() {
  if [ ! -e "$vibeshine_runtime_root" ] && [ ! -L "$vibeshine_runtime_root" ]; then return 0; fi
  [ -d "$vibeshine_runtime_root" ] && [ ! -L "$vibeshine_runtime_root" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibeshine_runtime_root")" = '0:0:755:directory' ]
}
vibeshine_legacy_handoff_process_is_running() {
  for vibeshine_proc in /proc/[0-9]*; do
    [ -d "$vibeshine_proc" ] || continue
    if grep -Fzxq -- "$vibeshine_legacy_handoff" "$vibeshine_proc/cmdline" 2>/dev/null; then return 0; fi
  done
  return 1
}
vibeshine_wait_for_legacy_handoff() {
  vibeshine_handoff_attempt=0
  vibeshine_handoff_clean_passes=0
  while [ "$vibeshine_handoff_attempt" -lt 400 ]; do
    if vibeshine_legacy_handoff_process_is_running; then
      vibeshine_handoff_clean_passes=0
    else
      vibeshine_handoff_clean_passes=$((vibeshine_handoff_clean_passes + 1))
      [ "$vibeshine_handoff_clean_passes" -lt 10 ] || return 0
    fi
    vibeshine_handoff_attempt=$((vibeshine_handoff_attempt + 1))
    sleep 0.1
  done
  return 1
}
vibeshine_disable_legacy_handoff() {
  if [ ! -e "$vibeshine_legacy_handoff" ] && [ ! -L "$vibeshine_legacy_handoff" ]; then return 0; fi
  [ -f "$vibeshine_legacy_handoff" ] && [ ! -L "$vibeshine_legacy_handoff" ] || return 1
  vibeshine_handoff_attributes=$(stat -c '%%u:%%g:%%a:%%h' -- "$vibeshine_legacy_handoff") || return 1
  case "$vibeshine_handoff_attributes" in
    '0:0:755:1' | '0:0:0:1') ;;
    *) return 1 ;;
  esac
  vibeshine_legacy_handoff_identity=$(stat -Lc '%%d:%%i' -- "$vibeshine_legacy_handoff") || return 1
  chmod 000 -- "$vibeshine_legacy_handoff" || return 1
  vibeshine_handoff_attributes=$(stat -Lc '%%d:%%i:%%u:%%g:%%a:%%h' -- \
    "$vibeshine_legacy_handoff") || return 1
  [ "$vibeshine_handoff_attributes" = \
    "$vibeshine_legacy_handoff_identity:0:0:0:1" ] || return 1
  vibeshine_wait_for_legacy_handoff
}
vibeshine_legacy_state_file_is_safe() {
  vibeshine_legacy_state_path=$1
  vibeshine_legacy_state_name=${vibeshine_legacy_state_path##*/}
  case "$vibeshine_legacy_state_name" in [1-9]*) ;; *) return 1 ;; esac
  case "$vibeshine_legacy_state_name" in *[!0-9]*) return 1 ;; esac
  [ -f "$vibeshine_legacy_state_path" ] && [ ! -L "$vibeshine_legacy_state_path" ] || return 1
  vibeshine_legacy_state_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%s' -- \
    "$vibeshine_legacy_state_path") || return 1
  case "$vibeshine_legacy_state_attributes" in
    0:0:600:1:* | 0:0:644:1:*) ;;
    *) return 1 ;;
  esac
  vibeshine_legacy_state_size=${vibeshine_legacy_state_attributes##*:}
  case "$vibeshine_legacy_state_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibeshine_legacy_state_size" -le 4096 ] || return 1
  {
    IFS= read -r vibeshine_legacy_state_user &&
      IFS= read -r vibeshine_legacy_state_uid &&
      IFS= read -r vibeshine_legacy_state_token &&
      ! IFS= read -r vibeshine_legacy_state_extra
  } <"$vibeshine_legacy_state_path" || return 1
  case "$vibeshine_legacy_state_user" in [a-z_]*) ;; *) return 1 ;; esac
  case "$vibeshine_legacy_state_user" in *[!a-z0-9_-]*) return 1 ;; esac
  [ "$vibeshine_legacy_state_uid" = "$vibeshine_legacy_state_name" ] || return 1
  [ "${#vibeshine_legacy_state_token}" -eq 32 ] || return 1
  case "$vibeshine_legacy_state_token" in *[!0-9a-f]*) return 1 ;; esac
}
vibeshine_remove_legacy_state_directory() {
  vibeshine_legacy_directory=$1
  if [ ! -e "$vibeshine_legacy_directory" ] && [ ! -L "$vibeshine_legacy_directory" ]; then return 0; fi
  [ -d "$vibeshine_legacy_directory" ] && [ ! -L "$vibeshine_legacy_directory" ] || return 1
  vibeshine_legacy_directory_attributes=$(stat -c '%%u:%%g:%%a' -- \
    "$vibeshine_legacy_directory") || return 1
  case "$vibeshine_legacy_directory_attributes" in
    '0:0:700' | '0:0:755') ;;
    *) return 1 ;;
  esac
  for vibeshine_legacy_state_path in "$vibeshine_legacy_directory"/* \
    "$vibeshine_legacy_directory"/.[!.]* "$vibeshine_legacy_directory"/..?*; do
    if [ ! -e "$vibeshine_legacy_state_path" ] && [ ! -L "$vibeshine_legacy_state_path" ]; then continue; fi
    vibeshine_legacy_state_file_is_safe "$vibeshine_legacy_state_path" || return 1
    rm -f -- "$vibeshine_legacy_state_path" || return 1
  done
  rmdir -- "$vibeshine_legacy_directory" || return 1
  [ ! -e "$vibeshine_legacy_directory" ] && [ ! -L "$vibeshine_legacy_directory" ]
}
vibeshine_remove_legacy_prelogin_marker() {
  if [ ! -e "$vibeshine_legacy_prelogin_marker" ] && [ ! -L "$vibeshine_legacy_prelogin_marker" ]; then return 0; fi
  [ -f "$vibeshine_legacy_prelogin_marker" ] && [ ! -L "$vibeshine_legacy_prelogin_marker" ] || return 1
  vibeshine_prelogin_marker_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%s' -- \
    "$vibeshine_legacy_prelogin_marker") || return 1
  case "$vibeshine_prelogin_marker_attributes" in 0:0:644:1:*) ;; *) return 1 ;; esac
  vibeshine_prelogin_marker_size=${vibeshine_prelogin_marker_attributes##*:}
  case "$vibeshine_prelogin_marker_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibeshine_prelogin_marker_size" -le 256 ] || return 1
  {
    IFS= read -r vibeshine_prelogin_marker_user &&
      ! IFS= read -r vibeshine_prelogin_marker_extra
  } <"$vibeshine_legacy_prelogin_marker" || return 1
  case "$vibeshine_prelogin_marker_user" in [a-z_]*) ;; *) return 1 ;; esac
  case "$vibeshine_prelogin_marker_user" in *[!a-z0-9_-]*) return 1 ;; esac
  rm -f -- "$vibeshine_legacy_prelogin_marker" || return 1
  [ ! -e "$vibeshine_legacy_prelogin_marker" ] && [ ! -L "$vibeshine_legacy_prelogin_marker" ]
}
vibeshine_cleanup_legacy_transition_state() {
  (
    vibeshine_runtime_root_is_safe_or_absent || exit 1
    if [ -e "$vibeshine_legacy_transition_lock" ] || [ -L "$vibeshine_legacy_transition_lock" ]; then
      [ -f "$vibeshine_legacy_transition_lock" ] && [ ! -L "$vibeshine_legacy_transition_lock" ] || exit 1
      [ "$(stat -c '%%u:%%g:%%a:%%h:%%s' -- "$vibeshine_legacy_transition_lock")" = \
        '0:0:600:1:0' ] || exit 1
      exec 9<>"$vibeshine_legacy_transition_lock" || exit 1
      timeout --signal=KILL 10 flock --exclusive 9 || exit 1
    fi
    vibeshine_wait_for_legacy_handoff || exit 1
    vibeshine_remove_legacy_state_directory "$vibeshine_legacy_handoff_directory" || exit 1
    vibeshine_remove_legacy_state_directory "$vibeshine_legacy_restore_directory" || exit 1
    vibeshine_remove_legacy_prelogin_marker || exit 1
    if [ -e "$vibeshine_legacy_transition_lock" ] || [ -L "$vibeshine_legacy_transition_lock" ]; then
      rm -f -- "$vibeshine_legacy_transition_lock" || exit 1
    fi
    [ ! -e "$vibeshine_legacy_transition_lock" ] && [ ! -L "$vibeshine_legacy_transition_lock" ]
  )
}
vibeshine_quiesce_machine_host() {
  vibeshine_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibeshine_have_systemd=1
    vibeshine_select_upgrade_kill_mode || return 1
    vibeshine_prepare_host_upgrade_fence || return 1
    vibeshine_freeze_controller || return 1
    vibeshine_host_is_stable_or_quiescent || return 1
    vibeshine_activate_host_upgrade_fence || return 1
    vibeshine_controller_remains_frozen || return 1
    vibeshine_host_is_stable_or_quiescent || return 1
    timeout --signal=KILL 15 systemctl mask --runtime \
      'vibeshine-session-restore@.service' 2>/dev/null || return 1
    vibeshine_restore_template_is_masked || return 1
    vibeshine_disable_legacy_handoff || return 1
    timeout --signal=KILL 15 systemctl mask --runtime vibeshine-vkms-control.socket 2>/dev/null || return 1
    vibeshine_control_socket_is_masked || return 1
    vibeshine_stop_exact_unit vibeshine-vkms-control.socket
    vibeshine_unit_is_quiescent vibeshine-vkms-control.socket || return 1
    vibeshine_control_instances_are_quiescent || return 1
    timeout --signal=KILL 15 systemctl mask --runtime vibeshine-session-exec.socket 2>/dev/null || return 1
    vibeshine_broker_socket_is_masked || return 1
    vibeshine_stop_exact_unit vibeshine-session-exec.socket
    vibeshine_unit_is_quiescent vibeshine-session-exec.socket || return 1
    vibeshine_stop_exact_unit vibeshine.service
    vibeshine_unit_is_quiescent vibeshine.service || return 1
    timeout --signal=KILL 15 systemctl mask --runtime vibeshine.service 2>/dev/null || return 1
    vibeshine_host_unit_is_masked || return 1
    vibeshine_stop_restore_instances || return 1
    vibeshine_stop_brokers || return 1
    vibeshine_controller_remains_frozen || return 1
    # systemd refuses StopUnit for a frozen service. Admission and the host are
    # already masked here, so thaw the controller only for its ordered stop.
    vibeshine_thaw_controller || return 1
    vibeshine_stop_exact_unit vibeshine-session-controller.service
    vibeshine_unit_is_quiescent vibeshine-session-controller.service || return 1
    vibeshine_stop_exact_unit vibeshine-prelogin.service
    vibeshine_stop_exact_unit vibeshine-machine-prepare.service
    timeout --signal=KILL 15 systemctl disable vibeshine-session-controller.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibeshine.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibeshine-prelogin.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibeshine-machine-prepare.service --now 2>/dev/null || true
  fi

  if [ "$vibeshine_have_systemd" -eq 0 ]; then vibeshine_disable_legacy_handoff || return 1; fi
  vibeshine_runtime_root_is_safe_or_absent || return 1

  if [ "$vibeshine_have_systemd" -eq 0 ] && \
     { [ -e "$vibeshine_controller" ] || [ -L "$vibeshine_controller" ]; }; then
    vibeshine_privileged_helper_is_safe "$vibeshine_controller" || return 1
    timeout --signal=KILL 40 "$vibeshine_controller" cleanup || return 1
  elif [ "$vibeshine_have_systemd" -eq 0 ]; then
    vibeshine_run_optional_legacy_command cleanup
    vibeshine_legacy_status=$?
    case "$vibeshine_legacy_status" in 0 | 2) ;; *) return 1 ;; esac
  fi

  vibeshine_run_optional_legacy_command remove-pam
  vibeshine_legacy_status=$?
  case "$vibeshine_legacy_status" in 0 | 2) ;; *) return 1 ;; esac

  if [ "$vibeshine_have_systemd" -eq 1 ]; then
    vibeshine_restore_template_is_masked || return 1
    vibeshine_host_unit_is_masked || return 1
    vibeshine_broker_socket_is_masked || return 1
    vibeshine_stop_restore_instances || return 1
    vibeshine_stop_exact_unit vibeshine.service
    vibeshine_unit_is_quiescent vibeshine.service || return 1
    vibeshine_stop_exact_unit vibeshine-session-exec.socket
    vibeshine_stop_brokers || return 1
    for vibeshine_unit in vibeshine-session-exec.socket vibeshine-session-controller.service \
      vibeshine.service vibeshine-prelogin.service vibeshine-machine-prepare.service; do
      vibeshine_stop_exact_unit "$vibeshine_unit"
      vibeshine_unit_is_quiescent "$vibeshine_unit" || return 1
      vibeshine_unit_is_disabled "$vibeshine_unit" || return 1
    done
    vibeshine_brokers_are_quiescent || return 1
    vibeshine_restore_instances_are_quiescent || return 1
  fi
  vibeshine_cleanup_legacy_transition_state || return 1
  if [ -S "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ]; then
    rm -f -- "$vibeshine_broker_socket" || return 1
  fi
  [ ! -e "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ] && \
    [ ! -e "$vibeshine_legacy_handoff_directory" ] && [ ! -L "$vibeshine_legacy_handoff_directory" ] && \
    [ ! -e "$vibeshine_legacy_restore_directory" ] && [ ! -L "$vibeshine_legacy_restore_directory" ] && \
    [ ! -e "$vibeshine_legacy_transition_lock" ] && [ ! -L "$vibeshine_legacy_transition_lock" ] && \
    [ ! -e "$vibeshine_legacy_prelogin_marker" ] && [ ! -L "$vibeshine_legacy_prelogin_marker" ]
}
if ! vibeshine_quiesce_machine_host; then
  echo "error: installed Vibeshine services did not quiesce; replacement is blocked and admission remains disabled." >&2
  exit 1
fi

%post
# Note: this is copied from the postinst script

vibeshine_controller=%{_prefix}/libexec/vibeshine/vibeshine-session-controller
vibeshine_session_record=/run/vibeshine/session.env
vibeshine_legacy_acl=/run/vibeshine/runtime-acl
vibeshine_broker_socket=/run/vibeshine/session-broker.sock
vibeshine_host_upgrade_dropin_dir=/run/systemd/system/vibeshine.service.d
vibeshine_host_upgrade_dropin=$vibeshine_host_upgrade_dropin_dir/90-vibeshine-safe-upgrade.conf
vibeshine_privileged_helper_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = \
    '0:0:755:1:regular file' ]
}
vibeshine_unmask_host_for_controller() {
  vibeshine_unit_is_quiescent vibeshine-session-controller.service || return 1
  vibeshine_unit_is_quiescent vibeshine-session-exec.socket || return 1
  if [ -e "$vibeshine_host_upgrade_dropin" ] || [ -L "$vibeshine_host_upgrade_dropin" ]; then
    [ -f "$vibeshine_host_upgrade_dropin" ] && [ ! -L "$vibeshine_host_upgrade_dropin" ] || return 1
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibeshine_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || return 1
    vibeshine_upgrade_contents=$(sed -n '1,6p' -- "$vibeshine_host_upgrade_dropin") || return 1
    case "$vibeshine_upgrade_contents" in
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no') ;;
      *) return 1 ;;
    esac
    rm -f -- "$vibeshine_host_upgrade_dropin" || return 1
    rmdir -- "$vibeshine_host_upgrade_dropin_dir" 2>/dev/null || true
  fi
  systemctl daemon-reload || return 1
  timeout --signal=KILL 15 systemctl unmask --runtime vibeshine.service 2>/dev/null || return 1
  systemctl daemon-reload || return 1
  vibeshine_new_host_properties=$(timeout --signal=KILL 5 systemctl show vibeshine.service \
    --property=RefuseManualStart --property=KillMode --property=SendSIGKILL 2>/dev/null) || return 1
  printf '%%s\n' "$vibeshine_new_host_properties" | grep -qx 'RefuseManualStart=no' && \
    printf '%%s\n' "$vibeshine_new_host_properties" | grep -qx 'KillMode=control-group' && \
    printf '%%s\n' "$vibeshine_new_host_properties" | grep -qx 'SendSIGKILL=no' || return 1
  timeout --signal=KILL 15 systemctl unmask --runtime vibeshine-vkms-control.socket 2>/dev/null || return 1
  systemctl start vibeshine-vkms-control.socket || return 1
  systemctl is-active --quiet vibeshine-vkms-control.socket || return 1
  timeout --signal=KILL 15 systemctl unmask --runtime vibeshine-session-exec.socket 2>/dev/null || return 1
  vibeshine_host_state=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine.service 2>/dev/null || true)
  vibeshine_host_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine.service --property=LoadState 2>/dev/null) || return 1
  vibeshine_socket_state=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine-session-exec.socket 2>/dev/null || true)
  vibeshine_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-session-exec.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_socket_state" in
    disabled | static | indirect | generated | transient | linked | linked-runtime)
      [ "$vibeshine_socket_load" = 'LoadState=loaded' ] || return 1 ;;
    *) return 1 ;;
  esac
  case "$vibeshine_host_state" in
    disabled | static | indirect | generated | transient | linked | linked-runtime)
      [ "$vibeshine_host_load" = 'LoadState=loaded' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_cgroup_is_quiescent() {
  vibeshine_control_group=$1
  [ -n "$vibeshine_control_group" ] || return 0
  case "$vibeshine_control_group" in /*) ;; *) return 1 ;; esac
  case "$vibeshine_control_group" in */../* | */..) return 1 ;; esac
  vibeshine_cgroup_path=/sys/fs/cgroup$vibeshine_control_group
  if [ ! -e "$vibeshine_cgroup_path" ] && [ ! -L "$vibeshine_cgroup_path" ]; then return 0; fi
  [ -d "$vibeshine_cgroup_path" ] && [ ! -L "$vibeshine_cgroup_path" ] && \
    [ -f "$vibeshine_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibeshine_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibeshine_cgroup_path/cgroup.events"
}
vibeshine_unit_is_quiescent() {
  vibeshine_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  [ "$(printf '%%s\n' "$vibeshine_properties" | wc -l | tr -d ' ')" = 5 ] || return 1
  for vibeshine_property in LoadState ActiveState SubState MainPID ControlGroup; do
    vibeshine_property_count=$(printf '%%s\n' "$vibeshine_properties" | \
      grep -c "^$vibeshine_property=" || true)
    [ "$vibeshine_property_count" = 1 ] || return 1
  done
  vibeshine_load=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^LoadState=//p')
  vibeshine_state=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^ActiveState=//p')
  vibeshine_substate=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^SubState=//p')
  vibeshine_pid=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^MainPID=//p')
  vibeshine_control_group=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibeshine_load:$vibeshine_state:$vibeshine_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibeshine_pid" = 0 ] && vibeshine_cgroup_is_quiescent "$vibeshine_control_group"
}
vibeshine_unit_is_disabled() {
  vibeshine_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  case "$vibeshine_enabled" in
    disabled | masked | masked-runtime | static | indirect | generated | transient | linked | linked-runtime | not-found) return 0 ;;
    *) return 1 ;;
  esac
}
vibeshine_broker_unit_is_safe() {
  vibeshine_broker_unit=$1
  case "$vibeshine_broker_unit" in vibeshine-session-exec@*.service) ;; *) return 1 ;; esac
  vibeshine_broker_instance=${vibeshine_broker_unit#vibeshine-session-exec@}
  vibeshine_broker_instance=${vibeshine_broker_instance%.service}
  [ -n "$vibeshine_broker_instance" ] || return 1
  case "$vibeshine_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibeshine_stop_exact_unit() {
  # Never bypass a service's ordered resource teardown.  Killing systemctl
  # only abandons the client while its manager job continues; killing the unit
  # cgroup can strand live GPU imports and is therefore forbidden here.
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}
vibeshine_bounded_broker_list() (
  vibeshine_broker_list=$(mktemp /run/vibeshine-broker-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibeshine_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  chmod 0600 "$vibeshine_broker_list" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units --all --plain \
      --no-legend --no-pager --full 'vibeshine-session-exec@*.service' \
      >"$vibeshine_broker_list" 2>/dev/null
  ) || return 1
  vibeshine_broker_list_size=$(stat -c '%%s' -- "$vibeshine_broker_list") || return 1
  case "$vibeshine_broker_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibeshine_broker_list_size" -le 65536 ] || return 1
  cat -- "$vibeshine_broker_list"
)
vibeshine_stop_brokers() (
  vibeshine_broker_attempt=0
  vibeshine_broker_clean_passes=0
  while [ "$vibeshine_broker_attempt" -lt 20 ]; do
    vibeshine_broker_dirty=0
    vibeshine_brokers=$(vibeshine_bounded_broker_list) || return 1
    vibeshine_seen_units='
'
    while IFS= read -r vibeshine_line || [ -n "$vibeshine_line" ]; do
      [ -n "$vibeshine_line" ] || continue
      case "$vibeshine_line" in *"
"*) return 1 ;; esac
      set -f
      set -- $vibeshine_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibeshine_unit=$1; vibeshine_load=$2
      vibeshine_state=$3; vibeshine_substate=$4
      vibeshine_broker_unit_is_safe "$vibeshine_unit" || return 1
      [ "$vibeshine_load" = loaded ] || return 1
      case "$vibeshine_state:$vibeshine_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibeshine_seen_units" in *"
$vibeshine_unit
"*) return 1 ;; esac
      vibeshine_seen_units="$vibeshine_seen_units$vibeshine_unit
"
      if ! vibeshine_unit_is_quiescent "$vibeshine_unit"; then
        vibeshine_broker_dirty=1
        vibeshine_stop_exact_unit "$vibeshine_unit"
        vibeshine_unit_is_quiescent "$vibeshine_unit" || return 1
      fi
    done <<EOF
$vibeshine_brokers
EOF
    if [ "$vibeshine_broker_dirty" -eq 0 ]; then
      vibeshine_broker_clean_passes=$((vibeshine_broker_clean_passes + 1))
      [ "$vibeshine_broker_clean_passes" -lt 5 ] || return 0
    else
      vibeshine_broker_clean_passes=0
    fi
    vibeshine_broker_attempt=$((vibeshine_broker_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibeshine_brokers_are_quiescent() {
  vibeshine_stop_brokers
}
vibeshine_quiesce_machine_host() {
  if ! command -v systemctl >/dev/null 2>&1 || [ ! -d /run/systemd/system ]; then
    [ ! -e "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ] && \
      [ ! -e "$vibeshine_session_record" ] && [ ! -L "$vibeshine_session_record" ] && \
      [ ! -e "$vibeshine_legacy_acl" ] && [ ! -L "$vibeshine_legacy_acl" ]
    return
  fi
  timeout --signal=KILL 15 systemctl mask --runtime vibeshine.service \
    vibeshine-session-exec.socket 2>/dev/null || return 1
  vibeshine_unit_is_masked vibeshine.service || return 1
  vibeshine_unit_is_masked vibeshine-session-exec.socket || return 1
  vibeshine_stop_exact_unit vibeshine-session-exec.socket
  vibeshine_unit_is_quiescent vibeshine-session-exec.socket || return 1
  vibeshine_stop_brokers || return 1
  for vibeshine_unit in vibeshine-session-controller.service vibeshine.service \
    vibeshine-prelogin.service vibeshine-machine-prepare.service; do
    vibeshine_stop_exact_unit "$vibeshine_unit"
  done
  timeout --signal=KILL 15 systemctl disable vibeshine-session-controller.service --now 2>/dev/null || true
  timeout --signal=KILL 15 systemctl disable vibeshine.service --now 2>/dev/null || true
  timeout --signal=KILL 15 systemctl disable vibeshine-prelogin.service --now 2>/dev/null || true
  timeout --signal=KILL 15 systemctl disable vibeshine-machine-prepare.service --now 2>/dev/null || true
  if [ -e "$vibeshine_controller" ] || [ -L "$vibeshine_controller" ]; then
    vibeshine_privileged_helper_is_safe "$vibeshine_controller" || return 1
    timeout --signal=KILL 40 "$vibeshine_controller" cleanup || return 1
  fi
  vibeshine_unit_is_masked vibeshine.service || return 1
  vibeshine_unit_is_masked vibeshine-session-exec.socket || return 1
  vibeshine_stop_exact_unit vibeshine-session-exec.socket
  vibeshine_stop_brokers || return 1
  for vibeshine_unit in vibeshine-session-exec.socket vibeshine-session-controller.service \
    vibeshine.service vibeshine-prelogin.service vibeshine-machine-prepare.service; do
    vibeshine_stop_exact_unit "$vibeshine_unit"
    vibeshine_unit_is_quiescent "$vibeshine_unit" || return 1
    vibeshine_unit_is_disabled "$vibeshine_unit" || return 1
  done
  vibeshine_stop_brokers || return 1
  if [ -S "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ]; then
    rm -f -- "$vibeshine_broker_socket" || return 1
  fi
  [ ! -e "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ] && \
    [ ! -e "$vibeshine_session_record" ] && [ ! -L "$vibeshine_session_record" ] && \
    [ ! -e "$vibeshine_legacy_acl" ] && [ ! -L "$vibeshine_legacy_acl" ]
}
if ! vibeshine_quiesce_machine_host; then
  echo "error: could not quiesce the machine host after package replacement; Vibeshine remains disabled." >&2
  exit 1
fi

for vibeshine_executable in \
  %{_bindir}/vibeshine \
  %{_prefix}/libexec/vibeshine/vibeshine-session-exec; do
  if [ ! -f "$vibeshine_executable" ] || [ -L "$vibeshine_executable" ]; then
    echo "error: native executable is missing or unsafe: $vibeshine_executable" >&2
    exit 1
  fi
  chown root:root "$vibeshine_executable" || exit 1
  chmod 0755 "$vibeshine_executable" || exit 1
  setcap -r "$vibeshine_executable" 2>/dev/null || true
  if [ -n "$(getcap "$vibeshine_executable" 2>/dev/null)" ]; then
    echo "error: unsafe capabilities remain on $vibeshine_executable." >&2
    exit 1
  fi
done

# Load uhid (DS5 emulation)
echo "Loading uhid kernel module for DS5 emulation."
modprobe uhid

# Check if we're in an rpm-ostree environment
if [ ! -x "$(command -v rpm-ostree)" ]; then
  echo "Not in an rpm-ostree environment, proceeding with post install steps."

  systemd-sysusers %{_prefix}/lib/sysusers.d/vibeshine-vkms.conf || \
    echo "warning: could not create the dedicated vibeshine-vkms control group."
  systemd-sysusers %{_prefix}/lib/sysusers.d/vibeshine.conf || {
    echo "error: could not create the dedicated Vibeshine service account." >&2
    exit 1
  }
  vibeshine_session_broker=%{_prefix}/libexec/vibeshine/vibeshine-session-broker
  if [ ! -f "$vibeshine_session_broker" ] || [ -L "$vibeshine_session_broker" ]; then
    echo "error: root-only session broker is missing or unsafe." >&2
    exit 1
  fi
  chown root:root "$vibeshine_session_broker" || exit 1
  chmod 0700 "$vibeshine_session_broker" || exit 1
  setcap cap_kill,cap_setgid,cap_setuid=p "$vibeshine_session_broker" || exit 1
  if [ "$(stat -c '%%U:%%G:%%a:%%F' -- "$vibeshine_session_broker")" != \
       'root:root:700:regular file' ] ||
     [ "$(getcap "$vibeshine_session_broker" 2>/dev/null)" != \
       "$vibeshine_session_broker cap_kill,cap_setgid,cap_setuid=p" ]; then
    echo "error: root-only session broker permissions or capabilities are unsafe." >&2
    exit 1
  fi

  vibeshine_private_host=%{_prefix}/libexec/vibeshine/vibeshine-host
  if [ ! -f "$vibeshine_private_host" ] || [ -L "$vibeshine_private_host" ]; then
    echo "error: private Vibeshine host is missing or unsafe." >&2
    exit 1
  fi
  vibeshine_public_identity=$(stat -Lc '%%d:%%i' -- %{_bindir}/vibeshine) || exit 1
  vibeshine_broker_identity=$(stat -Lc '%%d:%%i' -- "$vibeshine_session_broker") || exit 1
  vibeshine_private_identity=$(stat -Lc '%%d:%%i' -- "$vibeshine_private_host") || exit 1
  if [ "$vibeshine_public_identity" = "$vibeshine_private_identity" ] ||
     [ "$vibeshine_broker_identity" = "$vibeshine_private_identity" ]; then
    echo "error: privileged broker and private/public hosts must be distinct inodes." >&2
    exit 1
  fi
  chown root:vibeshine "$vibeshine_private_host" || exit 1
  chmod 0750 "$vibeshine_private_host" || exit 1
  setcap cap_sys_admin,cap_sys_nice=p "$vibeshine_private_host" || exit 1
  if [ "$(stat -c '%%U:%%G:%%a:%%F' -- "$vibeshine_private_host")" != \
       'root:vibeshine:750:regular file' ] ||
     [ "$(getcap "$vibeshine_private_host" 2>/dev/null)" != \
       "$vibeshine_private_host cap_sys_admin,cap_sys_nice=p" ]; then
    echo "error: private Vibeshine host permissions or capabilities are unsafe." >&2
    exit 1
  fi
  if [ "$(getcap "$vibeshine_session_broker" 2>/dev/null)" != \
       "$vibeshine_session_broker cap_kill,cap_setgid,cap_setuid=p" ]; then
    echo "error: private-host setup altered the session broker capabilities." >&2
    exit 1
  fi
  for vibeshine_executable in \
    %{_bindir}/vibeshine \
    %{_prefix}/libexec/vibeshine/vibeshine-session-exec; do
    if [ -n "$(getcap "$vibeshine_executable" 2>/dev/null)" ]; then
      echo "error: a public Vibeshine entrypoint gained file capabilities: $vibeshine_executable" >&2
      exit 1
    fi
  done

  # Trigger udev rule reload for /dev/uinput and /dev/uhid
  path_to_udevadm=$(command -v udevadm 2>/dev/null || true)
  if [ -x "$path_to_udevadm" ]; then
    echo "Reloading udev rules."
    $path_to_udevadm control --reload-rules
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uinput
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uhid
    echo "Udev rules reloaded successfully."
  else
    echo "error: udevadm not found or not executable."
  fi

  vibeshine_restore_kwin_capability() {
    # Earlier Vibeshine builds removed cap_sys_nice from the distro KWin binary
    # so the GPU bridge could be preloaded. The bridge is now a trusted
    # set-user-ID library, so give KWin its realtime capability back.
    kwin=/usr/bin/kwin_wayland
    marker=user.vibeshine.cap_sys_nice_removed
    timeout --signal=KILL 15 systemctl disable vibeshine-kwin-capability.path --now 2>/dev/null || true
    rm -f /etc/systemd/system/multi-user.target.wants/vibeshine-kwin-capability.path
    [ -f "$kwin" ] && [ ! -L "$kwin" ] || return 0
    if command -v getfattr >/dev/null 2>&1; then
      getfattr -n "$marker" --only-values "$kwin" >/dev/null 2>&1 || return 0
    elif command -v python3 >/dev/null 2>&1; then
      python3 -c 'import os, sys; os.getxattr(sys.argv[1], sys.argv[2])' "$kwin" "$marker" 2>/dev/null || return 0
    else
      return 0
    fi
    if setcap cap_sys_nice=ep "$kwin"; then
      setfattr -x "$marker" "$kwin" 2>/dev/null || \
        python3 -c 'import os, sys; os.removexattr(sys.argv[1], sys.argv[2])' "$kwin" "$marker" 2>/dev/null || true
      echo "restored cap_sys_nice on $kwin (removed by an earlier Vibeshine build)"
    else
      echo "warning: could not restore cap_sys_nice on $kwin; reinstall the kwin package." >&2
    fi
  }
  vibeshine_restore_kwin_capability || true

  if %{_prefix}/libexec/vibeshine/vibeshine-drm-install install; then
    :
  else
    vibeshine_drm_rc=$?
    if [ "$vibeshine_drm_rc" -eq 4 ]; then
      echo "warning: Vibeshine DRM was updated, but the loaded module is stale; reboot before using managed virtual displays."
    else
      echo "warning: Vibeshine DRM installation failed; managed virtual displays are unavailable."
    fi
  fi
  vibeshine_machine_helper=%{_prefix}/libexec/vibeshine/vibeshine-machine-host
  vibeshine_privileged_helper_is_safe "$vibeshine_machine_helper" || {
    echo "error: installed Vibeshine machine helper is unsafe." >&2
    exit 1
  }
  "$vibeshine_machine_helper" remove-pam || \
    echo "warning: could not remove the obsolete Plasma Login Manager handoff hook."
  systemctl disable --now vibeshine-prelogin.service 2>/dev/null || true
  systemctl daemon-reload || exit 1
  if "$vibeshine_machine_helper" configure-auto; then
    if ! systemctl enable vibeshine-session-controller.service; then
      vibeshine_quiesce_machine_host || true
      echo "error: could not enable the Vibeshine controller safely; all machine-host units remain off." >&2
      exit 1
    fi
    if ! vibeshine_unmask_host_for_controller || \
       ! systemctl start vibeshine-session-controller.service; then
      timeout --signal=KILL 15 systemctl mask --runtime vibeshine.service 2>/dev/null || true
      vibeshine_quiesce_machine_host || true
      echo "error: could not start the Vibeshine controller safely; all machine-host units remain off." >&2
      exit 1
    fi
  else
    echo "==> ACTION REQUIRED: Vibeshine could not prepare the machine profile; review the preceding setup or migration error." >&2
    echo "    Run:  sudo vibeshine configure USER" >&2
    echo "    then: sudo systemctl enable --now vibeshine-session-controller.service" >&2
  fi
else
  echo "rpm-ostree environment detected, skipping post install steps. Restart to apply the changes."
fi

%preun
vibeshine_controller=%{_prefix}/libexec/vibeshine/vibeshine-session-controller
vibeshine_machine_host=%{_prefix}/libexec/vibeshine/vibeshine-machine-host
vibeshine_session_record=/run/vibeshine/session.env
vibeshine_legacy_acl=/run/vibeshine/runtime-acl
vibeshine_broker_socket=/run/vibeshine/session-broker.sock
vibeshine_preun_privileged_helper_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = \
    '0:0:755:1:regular file' ]
}
vibeshine_preun_cgroup_is_quiescent() {
  vibeshine_control_group=$1
  [ -n "$vibeshine_control_group" ] || return 0
  case "$vibeshine_control_group" in /*) ;; *) return 1 ;; esac
  case "$vibeshine_control_group" in */../* | */..) return 1 ;; esac
  vibeshine_cgroup_path=/sys/fs/cgroup$vibeshine_control_group
  if [ ! -e "$vibeshine_cgroup_path" ] && [ ! -L "$vibeshine_cgroup_path" ]; then return 0; fi
  [ -d "$vibeshine_cgroup_path" ] && [ ! -L "$vibeshine_cgroup_path" ] && \
    [ -f "$vibeshine_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibeshine_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibeshine_cgroup_path/cgroup.events"
}
vibeshine_preun_unit_is_quiescent() {
  vibeshine_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  [ "$(printf '%%s\n' "$vibeshine_properties" | wc -l | tr -d ' ')" = 5 ] || return 1
  for vibeshine_property in LoadState ActiveState SubState MainPID ControlGroup; do
    vibeshine_property_count=$(printf '%%s\n' "$vibeshine_properties" | \
      grep -c "^$vibeshine_property=" || true)
    [ "$vibeshine_property_count" = 1 ] || return 1
  done
  vibeshine_load=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^LoadState=//p')
  vibeshine_state=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^ActiveState=//p')
  vibeshine_substate=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^SubState=//p')
  vibeshine_pid=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^MainPID=//p')
  vibeshine_control_group=$(printf '%%s\n' "$vibeshine_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibeshine_load:$vibeshine_state:$vibeshine_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibeshine_pid" = 0 ] && vibeshine_preun_cgroup_is_quiescent "$vibeshine_control_group"
}
vibeshine_preun_unit_is_disabled() {
  vibeshine_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  case "$vibeshine_enabled" in
    disabled | masked | masked-runtime | static | indirect | generated | transient | linked | linked-runtime | not-found) return 0 ;;
    *) return 1 ;;
  esac
}
vibeshine_preun_unit_is_masked() {
  vibeshine_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  vibeshine_load=$(timeout --signal=KILL 5 systemctl show "$1" --property=LoadState 2>/dev/null) || return 1
  case "$vibeshine_enabled" in
    masked | masked-runtime) [ "$vibeshine_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibeshine_preun_broker_unit_is_safe() {
  vibeshine_broker_unit=$1
  case "$vibeshine_broker_unit" in vibeshine-session-exec@*.service) ;; *) return 1 ;; esac
  vibeshine_broker_instance=${vibeshine_broker_unit#vibeshine-session-exec@}
  vibeshine_broker_instance=${vibeshine_broker_instance%.service}
  [ -n "$vibeshine_broker_instance" ] || return 1
  case "$vibeshine_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibeshine_preun_stop_exact_unit() {
  # Never bypass a service's ordered resource teardown.  Killing systemctl
  # only abandons the client while its manager job continues; killing the unit
  # cgroup can strand live GPU imports and is therefore forbidden here.
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}
vibeshine_preun_bounded_broker_list() (
  vibeshine_broker_list=$(mktemp /run/vibeshine-broker-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibeshine_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  chmod 0600 "$vibeshine_broker_list" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units --all --plain \
      --no-legend --no-pager --full 'vibeshine-session-exec@*.service' \
      >"$vibeshine_broker_list" 2>/dev/null
  ) || return 1
  vibeshine_broker_list_size=$(stat -c '%%s' -- "$vibeshine_broker_list") || return 1
  case "$vibeshine_broker_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibeshine_broker_list_size" -le 65536 ] || return 1
  cat -- "$vibeshine_broker_list"
)
vibeshine_preun_stop_brokers() (
  vibeshine_broker_attempt=0
  vibeshine_broker_clean_passes=0
  while [ "$vibeshine_broker_attempt" -lt 20 ]; do
    vibeshine_broker_dirty=0
    vibeshine_brokers=$(vibeshine_preun_bounded_broker_list) || return 1
    vibeshine_seen_units='
'
    while IFS= read -r vibeshine_line || [ -n "$vibeshine_line" ]; do
      [ -n "$vibeshine_line" ] || continue
      case "$vibeshine_line" in *"
"*) return 1 ;; esac
      set -f
      set -- $vibeshine_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibeshine_unit=$1; vibeshine_load=$2
      vibeshine_state=$3; vibeshine_substate=$4
      vibeshine_preun_broker_unit_is_safe "$vibeshine_unit" || return 1
      [ "$vibeshine_load" = loaded ] || return 1
      case "$vibeshine_state:$vibeshine_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibeshine_seen_units" in *"
$vibeshine_unit
"*) return 1 ;; esac
      vibeshine_seen_units="$vibeshine_seen_units$vibeshine_unit
"
      if ! vibeshine_preun_unit_is_quiescent "$vibeshine_unit"; then
        vibeshine_broker_dirty=1
        vibeshine_preun_stop_exact_unit "$vibeshine_unit"
        vibeshine_preun_unit_is_quiescent "$vibeshine_unit" || return 1
      fi
    done <<EOF
$vibeshine_brokers
EOF
    if [ "$vibeshine_broker_dirty" -eq 0 ]; then
      vibeshine_broker_clean_passes=$((vibeshine_broker_clean_passes + 1))
      [ "$vibeshine_broker_clean_passes" -lt 5 ] || return 0
    else
      vibeshine_broker_clean_passes=0
    fi
    vibeshine_broker_attempt=$((vibeshine_broker_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibeshine_preun_remove_pam() {
  if [ ! -e "$vibeshine_machine_host" ] && [ ! -L "$vibeshine_machine_host" ]; then return 0; fi
  vibeshine_preun_privileged_helper_is_safe "$vibeshine_machine_host" || return 1
  timeout --signal=KILL 40 "$vibeshine_machine_host" remove-pam >/dev/null 2>&1
}
vibeshine_preun_quiesce() {
  vibeshine_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibeshine_have_systemd=1
    timeout --signal=KILL 15 systemctl mask --runtime vibeshine.service \
      vibeshine-session-exec.socket 2>/dev/null || return 1
    vibeshine_preun_unit_is_masked vibeshine.service || return 1
    vibeshine_preun_unit_is_masked vibeshine-session-exec.socket || return 1
    vibeshine_preun_stop_exact_unit vibeshine-session-exec.socket
    vibeshine_preun_unit_is_quiescent vibeshine-session-exec.socket || return 1
    vibeshine_preun_stop_brokers || return 1
    for vibeshine_unit in vibeshine-session-controller.service vibeshine.service \
      vibeshine-prelogin.service vibeshine-machine-prepare.service; do
      vibeshine_preun_stop_exact_unit "$vibeshine_unit"
    done
    timeout --signal=KILL 15 systemctl disable vibeshine-session-controller.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibeshine.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibeshine-prelogin.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibeshine-machine-prepare.service --now 2>/dev/null || true
  fi
  if [ -e "$vibeshine_controller" ] || [ -L "$vibeshine_controller" ]; then
    vibeshine_preun_privileged_helper_is_safe "$vibeshine_controller" || return 1
    [ "$vibeshine_have_systemd" -eq 1 ] || return 1
    timeout --signal=KILL 40 "$vibeshine_controller" cleanup || return 1
  fi
  vibeshine_preun_remove_pam || return 1
  if [ "$vibeshine_have_systemd" -eq 1 ]; then
    vibeshine_preun_unit_is_masked vibeshine.service || return 1
    vibeshine_preun_unit_is_masked vibeshine-session-exec.socket || return 1
    vibeshine_preun_stop_exact_unit vibeshine-session-exec.socket
    vibeshine_preun_stop_brokers || return 1
    for vibeshine_unit in vibeshine-session-exec.socket vibeshine-session-controller.service \
      vibeshine.service vibeshine-prelogin.service vibeshine-machine-prepare.service; do
      vibeshine_preun_stop_exact_unit "$vibeshine_unit"
      vibeshine_preun_unit_is_quiescent "$vibeshine_unit" || return 1
      vibeshine_preun_unit_is_disabled "$vibeshine_unit" || return 1
    done
    vibeshine_preun_stop_brokers || return 1
  fi
  if [ -S "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ]; then
    rm -f -- "$vibeshine_broker_socket" || return 1
  fi
  [ ! -e "$vibeshine_broker_socket" ] && [ ! -L "$vibeshine_broker_socket" ] && \
    [ ! -e "$vibeshine_session_record" ] && [ ! -L "$vibeshine_session_record" ] && \
    [ ! -e "$vibeshine_legacy_acl" ] && [ ! -L "$vibeshine_legacy_acl" ]
}
if [ "$1" -eq 0 ]; then
  vibeshine_preun_quiesce || {
    echo "error: refusing to uninstall while Vibeshine cgroups or session state remain." >&2
    exit 1
  }
  timeout --signal=KILL 30 systemctl stop vibeshine-vkms.service 2>/dev/null || true
  timeout --signal=KILL 30 systemctl stop vibeshine-drm-setup.service 2>/dev/null || true
  %{_prefix}/libexec/vibeshine/vibeshine-drm-install remove || \
    echo "warning: could not remove the Vibeshine HDR DRM module cleanly."
fi

%files
# Executables
%{_bindir}/vibeshine
%{_bindir}/vibeshine-mangohud
%{_prefix}/libexec/vibeshine/vibeshine-drm-install
%{_prefix}/libexec/vibeshine/vibeshine-vkms
%{_prefix}/libexec/vibeshine/vibeshine-vkms-quiesce
%{_prefix}/libexec/vibeshine/vibeshine-vkms-peercred
%{_prefix}/libexec/vibeshine/vibeshine-session-controller
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibeshine-session-exec
%attr(0700,root,root) %caps(cap_kill,cap_setgid,cap_setuid+p) %{_prefix}/libexec/vibeshine/vibeshine-session-broker
%{_prefix}/libexec/vibeshine/vibeshine-provider-scan
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibeshine-steam-launch
%{_prefix}/libexec/vibeshine/vibeshine-profile-import
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibeshine-app-supervisor
%{_prefix}/libexec/vibeshine/vibeshine-machine-host
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibeshine-kwin-session-environment
%attr(0750,root,vibeshine) %caps(cap_sys_admin,cap_sys_nice+p) %{_prefix}/libexec/vibeshine/vibeshine-host
%attr(4755,root,root) %{_libdir}/libvibeshine-kwin-gpu.so

# Dedicated access group for the privileged virtual-display control socket
%{_prefix}/lib/sysusers.d/vibeshine-vkms.conf
%{_prefix}/lib/sysusers.d/vibeshine.conf

# Versioned DKMS/direct-build source tree
/usr/src/vibeshine-drm-*

# KWin user-unit drop-ins; Linux does not install the generic app service.
%{_userunitdir}/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf
%{_userunitdir}/plasma-kwin_wayland.service.d/vibeshine-kwin-session-environment.conf
%{_userunitdir}/plasma-login-kwin_wayland.service.d/vibeshine-kwin-gpu.conf
%{_userunitdir}/plasma-login-kwin_wayland.service.d/vibeshine-kwin-session-environment.conf

# Privileged virtual-display provisioning service
%{_unitdir}/vibeshine-drm-setup.service
%{_unitdir}/vibeshine-vkms-control.socket
%{_unitdir}/vibeshine-vkms-control@.service
%{_unitdir}/vibeshine-vkms.service
%{_unitdir}/vibeshine-session-exec.socket
%{_unitdir}/vibeshine-session-exec@.service
%{_unitdir}/vibeshine-session-controller.service
%{_unitdir}/vibeshine.service

# Udev rules
%{_udevrulesdir}/*-sunshine.rules

# Modules-load configuration
%{_modulesloaddir}/*-sunshine.conf

# Desktop entries
%{_datadir}/applications/*.desktop

# Icons
%{_datadir}/icons/hicolor/scalable/apps/io.github.Nonary.vibeshine.svg
%{_datadir}/icons/hicolor/scalable/status/io.github.Nonary.vibeshine-*.svg

# Metainfo
%{_datadir}/metainfo/*.metainfo.xml

# Assets
%{_datadir}/vibeshine/**

%changelog
