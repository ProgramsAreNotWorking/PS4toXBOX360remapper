{ config, lib, pkgs, ... }:

let
  cfg = config.services.ds4xbox;

  args = [
    "--quiet"
    "--profile" cfg.profile
  ]
  ++ lib.optional cfg.swapSticksTriggers "--swap-sticks-triggers"
  ++ lib.optional cfg.noForceFeedback "--no-ff"
  ++ cfg.extraArgs;

  execStart = lib.concatStringsSep " "
    ([ "${cfg.package}/bin/ds4xbox" ] ++ map lib.escapeShellArg args);
in
{
  options.services.ds4xbox = {
    enable = lib.mkEnableOption "the ds4xbox DualShock 4 / DualSense to Xbox remapper";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "pkgs.callPackage ./package.nix { }";
      description = "The ds4xbox package to use.";
    };

    profile = lib.mkOption {
      type = lib.types.enum [ "xbox360" "xboxone" ];
      default = "xbox360";
      description = "Virtual controller profile exposed to games.";
    };

    swapSticksTriggers = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Swap the right stick and trigger axes.  Needed for some third-party
        controllers that report ABS_Z/ABS_RZ as the right stick and
        ABS_RX/ABS_RY as the triggers.
      '';
    };

    noForceFeedback = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Disable force feedback (rumble) forwarding.";
    };

    extraArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      description = "Extra command-line arguments passed to ds4xbox.";
      example = [ "--device" "/dev/input/event20" "--force" ];
    };
  };

  config = lib.mkIf cfg.enable {
    boot.kernelModules = [ "uinput" ];

    services.udev.packages = [ cfg.package ];

    environment.systemPackages = [ cfg.package ];

    systemd.services.ds4xbox = {
      description = "ds4xbox - DualShock 4 / DualSense to Xbox 360/One remapper";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-udevd.service" ];
      wants = [ "systemd-udevd.service" ];

      serviceConfig = {
        Type = "simple";
        Environment = [ "PATH=${pkgs.systemd}/bin:/run/current-system/sw/bin:/usr/bin:/bin" ];
        ExecStart = execStart;
        Restart = "on-failure";
        RestartSec = 2;
        KillSignal = "SIGTERM";
        TimeoutStopSec = 5;
        ExecStopPost = "-/bin/sh -c 'rm -f /run/ds4xbox/active; ${pkgs.systemd}/bin/udevadm trigger --subsystem-match=input --action=change; ${pkgs.systemd}/bin/udevadm trigger --subsystem-match=hidraw --action=change; ${pkgs.systemd}/bin/udevadm trigger --subsystem-match=usb --action=change'";

        User = "root";
        Group = "root";

        RuntimeDirectory = "ds4xbox";
        RuntimeDirectoryMode = "0755";
        UMask = "0077";

        PrivateDevices = false;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        NoNewPrivileges = true;
        ReadWritePaths = [ "/run/ds4xbox" ];
        # udevadm trigger writes to /sys/.../uevent, so /sys must stay
        # writable for the hide/restore calls.
        ProtectKernelTunables = false;
        ProtectKernelModules = true;
        ProtectControlGroups = true;
        LockPersonality = true;
        MemoryDenyWriteExecute = true;
        RestrictAddressFamilies = [ "AF_UNIX" "AF_NETLINK" ];
        RestrictSUIDSGID = true;
        RemoveIPC = true;
      };
    };
  };
}
