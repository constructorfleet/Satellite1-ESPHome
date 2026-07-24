# Patched HTTP request component

This component is vendored from ESPHome commit
`ff8ce89556748509d7ee8724e12d9d43d3c8c1e8`, which contains the
`http_request` media source required by the Satellite1 media player.

Keeping it in the Satellite1 component tree prevents the full pinned ESPHome
repository from shadowing other external components during remote package
validation.
