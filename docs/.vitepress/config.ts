import { defineConfig } from "vitepress";

export default defineConfig({
  title: "OpenBallance",
  description: "A faithful reimplementation of Ballance (2004): the original Virtools scripts on a new runtime, using the data from your own CD.",
  cleanUrls: true,
  lastUpdated: true,
  srcExclude: ["README.md", "AGENTS.md"],
  sitemap: { hostname: "https://openballance.emdzej.pl" },
  // The browser player is a static page in public/play/, not a Markdown page.
  ignoreDeadLinks: [/^\/play\//],

  head: [
    ["link", { rel: "icon", href: "/favicon.png", type: "image/png" }],
    ["meta", { name: "theme-color", content: "#e8835a" }],
    ["meta", { property: "og:title", content: "OpenBallance: Ballance, rolling again" }],
    ["meta", { property: "og:description", content: "A faithful reimplementation of Ballance: the original game scripts on a new Virtools-compatible runtime with a port of its physics engine. Runs on gasm, natively and in the browser." }],
    ["meta", { property: "og:image", content: "https://openballance.emdzej.pl/screenshots/level01.jpg" }],
    ["meta", { property: "og:url", content: "https://openballance.emdzej.pl/" }],
  ],

  themeConfig: {
    siteTitle: "OpenBallance",
    logo: "/favicon.png",

    nav: [
      { text: "User guide", link: "/guide/", activeMatch: "/guide/" },
      { text: "How-tos", link: "/howto/build-from-source", activeMatch: "/howto/" },
      {
        text: "Internals",
        link: "/internals/",
        activeMatch: "/(internals|ck-runtime|gameplay_bbs|fonts|sky|animation|particles|physics|ivp_)",
      },
      { text: "Gallery", link: "/gallery" },
      // A static app in docs/public/play/: target _self makes it a full page load, not a VitePress route.
      { text: "Play", link: "/play/", target: "_self" },
    ],

    sidebar: {
      "/guide/": [
        {
          text: "User guide",
          items: [
            { text: "Introduction", link: "/guide/" },
            { text: "Status and compatibility", link: "/guide/status" },
            { text: "Installing", link: "/guide/install" },
            { text: "Game data", link: "/guide/game-data" },
            { text: "Running on gasm", link: "/guide/running" },
            { text: "Playing in the browser", link: "/guide/browser" },
            { text: "Controls", link: "/guide/controls" },
            { text: "Launch parameters", link: "/guide/parameters" },
            { text: "Troubleshooting", link: "/guide/troubleshooting" },
          ],
        },
      ],
      "/howto/": [
        {
          text: "How-tos",
          items: [
            { text: "Build from source", link: "/howto/build-from-source" },
            { text: "Run the tests", link: "/howto/tests" },
            { text: "Headless runs and screenshots", link: "/howto/headless" },
            { text: "Make a release", link: "/howto/release" },
            { text: "Reverse-engineering workflow", link: "/howto/reverse-engineering" },
          ],
        },
      ],
      "/": [
        {
          text: "Internals",
          items: [
            { text: "Overview", link: "/internals/" },
            { text: "Architecture", link: "/internals/architecture" },
            { text: "Virtools runtime (CK2)", link: "/ck-runtime" },
          ],
        },
        {
          text: "Building Blocks",
          items: [
            { text: "Gameplay BBs", link: "/gameplay_bbs" },
            { text: "Fonts and 2D text", link: "/fonts" },
            { text: "Sky (TT Sky)", link: "/sky" },
            { text: "Curves and animations", link: "/animation" },
            { text: "Particle systems", link: "/particles" },
          ],
        },
        {
          text: "Physics (Ipion)",
          items: [
            { text: "Physics BBs and manager", link: "/physics" },
            { text: "IVP core and dynamics", link: "/ivp_core" },
            { text: "IVP collision detection", link: "/ivp_collision" },
            { text: "IVP contact response", link: "/ivp_contact" },
          ],
        },
        {
          text: "Design records",
          items: [
            { text: "Initial analysis", link: "/internals/analysis" },
            { text: "gasm gfx requirements", link: "/internals/gasm-gfx-requirements" },
          ],
        },
      ],
    },

    socialLinks: [{ icon: "github", link: "https://github.com/emdzej/openballance" }],

    editLink: {
      pattern: "https://github.com/emdzej/openballance/edit/main/docs/:path",
      text: "Edit this page on GitHub",
    },

    outline: { level: [2, 3] },

    search: { provider: "local" },

    footer: {
      message:
        "OpenBallance is released under the GPL-3.0. Ballance is © 2004 Cyparade / Atari. OpenBallance contains no original code or assets: it runs the game from your own copy. It runs on <a href=\"https://gasm.emdzej.pl\">gasm</a>.",
    },
  },
});
