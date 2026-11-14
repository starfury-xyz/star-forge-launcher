# resources/

Launcher artwork for this pack, shown while its profile is selected (players
can turn this off in Settings). Every file is optional; whatever is missing
falls back to the STAR FORGE default. Point to them from `branding` in
`pack.json`.

| File | Used for | Format |
|---|---|---|
| `logo.png` | wordmark on the play screen | transparent PNG, about 690x246 |
| `icon.png` | pack list and side rail | transparent PNG, 256x256 square |
| `background.jpg` | play screen art | JPG, PNG or WebP (checked by content), up to 8 MB, at least 1920x1080; keep the subject right of centre, the logo and controls sit on the left |

`branding.background` can also be a list; the launcher picks one at random
each time it starts:

```json
"background": ["resources/bg-1.jpg", "resources/bg-2.jpg", "resources/bg-3.jpg"]
```

Keep the whole pack small (a few MB) so it downloads quickly.
