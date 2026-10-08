
{
  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.2";
    storage_module.url = "github:logos-co/logos-storage-module/v3.0.2";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
