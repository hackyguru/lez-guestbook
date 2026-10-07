{
  description = "guestbook_core — talks to the shared guestbook program on the LEZ v0.3 testnet";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Same LEZ commit the guestbook program and the testnet speak (v0.3.0 + r0 link fix).
    logos-execution-zone.url = "github:logos-blockchain/logos-execution-zone?rev=411adc8fdb3f4c3af64354c4fc26d3e4a6a3d3f4";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        wallet_ffi = {
          input = inputs.logos-execution-zone;
          packages.default = "wallet";
        };
      };
    };
}
