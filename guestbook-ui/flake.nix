{
  description = "guestbook — QML frontend for the guestbook_core module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # The core module from this same repo. To build against a local checkout:
    #   nix build --override-input guestbook_core path:../guestbook-core '.#lgx-portable'
    guestbook_core.url = "github:hackyguru/lez-guestbook?dir=guestbook-core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
