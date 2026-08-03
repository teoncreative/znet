//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// The authentication service, as seen from outside it. Stands in for whatever
// owns your accounts.
//
// Two audiences, and they are not the same:
//
//   a client calls RequestToken, which in production is an HTTPS round trip to
//   the service, made once at login and not per connection
//
//   a server calls ServicePublicKey, which in production is not a call at all
//   but a value in its config, pasted from the service's documentation. It is a
//   function here only so the example cannot drift out of sync with itself
//
// The signing key is in service.cc and is declared in no header, so no amount
// of including gets a client or a server near it. That is the whole reason this
// is a separate translation unit: in the real system the private half lives on
// another machine, and the example should not be able to pretend otherwise.
//

#pragma once

#include "token.h"

// The public half. All a server ever needs, and safe to hand to anyone.
// Returns kEd25519KeyLength bytes.
const unsigned char* ServicePublicKey();

// What a client gets after logging in. The service has authenticated the user
// by some means of its own by this point, which is exactly the part this example
// does not model: it says yes to everyone.
//
// A regular authentication service would take credentials before handing
// a token.
//
// The token names the client's public key, so the client must already hold the
// matching private key. That key never reaches the service, and never reaches
// the game server either.
bool RequestToken(const std::string& user_id,
                  const unsigned char* client_public_key, AuthToken* out);
