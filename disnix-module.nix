{pkgs, lib, config, ...}:

with lib;

let
  cfg = config.services.disnixTest;
in
{
  options = {
    services = {
      disnixTest = {
        enable = mkOption {
          type = types.bool;
          default = false;
          description = "Whether to enable Disnix";
        };
        
        package = mkOption {
          type = types.path;
          description = "The Disnix package";
        };
        
        dysnomia = mkOption {
          type = types.path;
          description = "The Dysnomia package";
        };
      };
    };
  };
  
  config = mkIf cfg.enable {
    ids.gids = { disnix = 200; };
    users.extraGroups = [ { gid = 200; name = "disnix"; } ];
    
    services.dbus.enable = true;
    services.dbus.packages = [ cfg.package ];
    services.openssh.enable = true;
    
    services.disnixTest.package = mkDefault (import ./release.nix {}).build."${builtins.currentSystem}";
    
    systemd.services.disnix =
      { description = "Disnix server";
        wantedBy = [ "multi-user.target" ];
        after = [ "dbus.service" ];
        
        path = [ config.nix.package cfg.package cfg.dysnomia ];
        environment = {
          HOME = "/root";
        };

        serviceConfig.ExecStart = "${cfg.package}/bin/disnix-service";
      };

    environment.systemPackages = [ cfg.package ];
  };
}
