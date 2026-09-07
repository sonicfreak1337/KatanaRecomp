# Independent native presentation

The parallel desktop host owns two color surfaces: a working title image and
the last completed image. Completing a frame swaps these surfaces. The render
thread composites the completed image on its own presentation deadline even
when the title thread produces no commands. No additional D3D owner is created.

The simulation clock remains independent. Sonic keeps its original 30-Hz
clock; 144-Hz presentation may repeat a completed image and does not claim
144 distinct animation frames. Draw, simulation and presentation counters
retain separate meanings, and successful repeats are counted separately.

Explicit Present and PresentImage commands output the completed image before
their ordered reply. Explicit RepeatPresent without a complete image retains
its InvalidFrame error. Idle deadlines may skip an open resource prefix:
compositing there would disturb unfinished Type-2 state. Consequently this
change alone does not guarantee uninterrupted 144-Hz output during prefixes.

Window pause or shutdown disables idle deadlines; resume starts a fresh epoch.
A reusable finish drain temporarily pauses output and resumes it on success,
so development checkpoint capture/restore does not permanently stop the clock.
Consumer errors stop autonomous presentation. SerialReference retains its
producer-driven behavior. Captures read the completed surface.

Compatibility: graphics contract 22, pacing contract 3, product runtime ABI
140. Generated SH-4 code uses unchanged AOT runtime ABI 128; this renderer
change alone must not invalidate cached AOT partitions.

Focused graphics tests cover idle repetition without title progress, exclusion
of partial resource frames, reusable drains, explicit invalid repeats and the
existing serial/error/Type-2 contracts. These are component checks. A Sonic
product run remains necessary to establish visual correctness, game progress
and measured output rate. API presentation counts do not prove physical
144-Hz display scanout.
