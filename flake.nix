
{
  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    storage_module.url = "github:logos-co/logos-storage-module?ref=refs/tags/v2.1.2";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}