/* Static reading aids; Doxygen retains ownership of search and clipboard behavior. */
(function () {
    "use strict";

    function unusedId(base) {
        let candidate = base;
        let suffix = 2;
        while (document.getElementById(candidate)) {
            candidate = base + "-" + suffix++;
        }
        return candidate;
    }

    function headingLabel(heading) {
        const copy = heading.cloneNode(true);
        copy.querySelectorAll("a.anchorlink").forEach(function (anchor) {
            anchor.remove();
        });
        return copy.textContent.replace(/\s+/g, " ").trim();
    }

    function headingId(heading) {
        const anchor = heading.querySelector("a.anchor[id]");
        return heading.id || (anchor && anchor.id) || "";
    }

    function trackContents(headings, content, details, wide) {
        let pendingFrame = null;
        let current = null;
        let contentsHeight = null;

        function update() {
            pendingFrame = null;
            const viewportHeight = document.documentElement.clientHeight;
            const contentStyle = window.getComputedStyle(content);
            const contentScrollRange = content.scrollHeight - content.clientHeight;
            const scrollsContent = /^(auto|scroll)$/.test(contentStyle.overflowY) && contentScrollRange > 1;
            const contentBounds = content.getBoundingClientRect();
            if (wide.matches) {
                const bottom = Math.min(viewportHeight,
                    contentBounds.top + content.clientTop + content.clientHeight);
                const top = Math.max(0, details.getBoundingClientRect().top);
                const available = Math.max(0, bottom - top - 16) + "px";
                if (available !== contentsHeight) {
                    details.style.setProperty("--reading-toc-available-height", available);
                    contentsHeight = available;
                }
            }
            let viewportTop = scrollsContent ? Math.max(0, contentBounds.top + content.clientTop) : 0;
            const viewportBottom = scrollsContent ?
                Math.min(viewportHeight, contentBounds.top + content.clientTop + content.clientHeight) : viewportHeight;
            const header = document.getElementById("top");
            if (header) {
                const position = window.getComputedStyle(header).position;
                const bounds = header.getBoundingClientRect();
                if ((position === "fixed" || position === "sticky") && bounds.top <= viewportTop) {
                    viewportTop = Math.max(viewportTop, bounds.bottom);
                }
            }
            const readingLine = viewportTop + Math.min(64, Math.max(0, viewportBottom - viewportTop) * 0.15);
            let selected = headings[0];
            for (const heading of headings) {
                if (heading.element.getBoundingClientRect().top > readingLine) {
                    break;
                }
                selected = heading;
            }
            const documentScroll = document.scrollingElement;
            const documentScrollRange = documentScroll ? documentScroll.scrollHeight - viewportHeight : 0;
            const atEnd = scrollsContent ? content.scrollTop >= contentScrollRange - 2 :
                documentScrollRange > 1 && documentScroll.scrollTop >= documentScrollRange - 2;
            if (atEnd) {
                selected = headings[headings.length - 1];
            }
            if (selected === current) {
                return;
            }
            if (current) {
                current.link.removeAttribute("aria-current");
            }
            selected.link.setAttribute("aria-current", "location");
            current = selected;
        }

        function schedule() {
            if (pendingFrame === null) {
                pendingFrame = window.requestAnimationFrame(update);
            }
        }

        content.addEventListener("scroll", schedule, { passive: true });
        window.addEventListener("scroll", schedule, { passive: true });
        window.addEventListener("hashchange", schedule);
        window.addEventListener("resize", schedule);
        window.addEventListener("load", schedule, { once: true });
        schedule();
    }

    function addContents(textblock) {
        const contents = textblock.parentElement;
        if (contents.children.length !== 1 || contents.firstElementChild !== textblock ||
            textblock.querySelector(".docs-home") || contents.querySelector(".toc, .reading-toc")) {
            return;
        }
        const headings = Array.from(textblock.querySelectorAll("h1, h2"))
            .map(function (heading) {
                return { element: heading, label: headingLabel(heading), id: headingId(heading) };
            })
            .filter(function (heading) { return heading.label.length > 0; });
        if (headings.filter(function (heading) { return heading.id.length > 0; }).length < 4) {
            return;
        }

        const details = document.createElement("details");
        details.className = "reading-toc";
        const summary = document.createElement("summary");
        summary.id = unusedId("reading-contents-title");
        summary.textContent = "On this page";
        details.appendChild(summary);
        const navigation = document.createElement("nav");
        navigation.setAttribute("aria-labelledby", summary.id);
        const list = document.createElement("ol");
        const hasTopLevel = headings.some(function (heading) { return heading.element.tagName === "H1"; });
        headings.forEach(function (heading) {
            if (!heading.id) {
                const slug = heading.label.toLowerCase().normalize("NFKD")
                    .replace(/[\u0300-\u036f]/g, "").replace(/[^a-z0-9]+/g, "-")
                    .replace(/^-|-$/g, "").slice(0, 80);
                heading.id = unusedId("section-" + (slug || "heading"));
                heading.element.id = heading.id;
            }
            const item = document.createElement("li");
            if (hasTopLevel && heading.element.tagName === "H2") {
                item.className = "reading-toc-subsection";
            }
            const link = document.createElement("a");
            link.href = "#" + encodeURIComponent(heading.id);
            link.textContent = heading.label;
            heading.link = link;
            item.appendChild(link);
            list.appendChild(item);
        });
        navigation.appendChild(list);
        details.appendChild(navigation);
        contents.insertBefore(details, textblock);
        contents.classList.add("reading-layout");
        const wide = window.matchMedia("(min-width: 1200px)");
        details.open = wide.matches;
        wide.addEventListener("change", function (event) { details.open = event.matches; });
        const content = textblock.closest("#doc-content");
        if (content) {
            trackContents(headings, content, details, wide);
        }
    }

    function isEditor(target) {
        return target instanceof Element && (target.isContentEditable ||
            target.closest("input, textarea, select, [contenteditable]:not([contenteditable='false']), [role='textbox'], .CodeMirror, .monaco-editor"));
    }

    function enableCopyButtons() {
        document.querySelectorAll("doxygen-awesome-fragment-copy-button").forEach(function (button) {
            if (button.dataset.readingAccessible) {
                return;
            }
            button.dataset.readingAccessible = "true";
            button.tabIndex = 0;
            button.setAttribute("role", "button");
            button.setAttribute("aria-label", "Copy code");
            button.querySelectorAll("svg").forEach(function (icon) {
                icon.setAttribute("aria-hidden", "true");
                icon.setAttribute("focusable", "false");
            });
            button.addEventListener("keydown", function (event) {
                if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    if (!event.repeat) {
                        button.click();
                    }
                }
            });
        });
    }

    function pageUrl(link) {
        const href = link.getAttribute("href");
        if (!href || href.startsWith("#")) {
            return null;
        }
        const url = new URL(href, document.baseURI);
        return /^(https?|file):$/.test(url.protocol) ? url : null;
    }

    function isCurrentPage(url) {
        return url && url.origin === window.location.origin &&
            url.pathname.replace(/\/index\.html$/, "/") === window.location.pathname.replace(/\/index\.html$/, "/");
    }

    function addPageContext(content) {
        const title = content.querySelector(".header .title");
        if (!title) {
            return;
        }
        if (!/^H[1-6]$/.test(title.tagName)) {
            title.setAttribute("role", "heading");
            title.setAttribute("aria-level", "1");
        }
        if (!title.id) {
            title.id = unusedId("reading-page-title");
        }
        const main = content.matches("main, [role='main']") ? content : document.querySelector("main, [role='main']");
        if (main && !main.hasAttribute("aria-label") && !main.hasAttribute("aria-labelledby")) {
            main.setAttribute("aria-labelledby", title.id);
        }
        if (content.querySelector(".docs-home")) {
            return;
        }

        const navigation = document.createElement("nav");
        navigation.className = "reading-breadcrumbs";
        navigation.setAttribute("aria-label", "Breadcrumb");
        const list = document.createElement("ol");
        const home = new URL("index.html", document.baseURI);
        const seen = new Set([home.href]);
        function appendLink(label, href) {
            const item = document.createElement("li");
            const link = document.createElement("a");
            link.textContent = label;
            link.href = href;
            item.appendChild(link);
            list.appendChild(item);
        }
        appendLink("Home", home.href);
        document.querySelectorAll("#nav-path li.navelem a").forEach(function (link) {
            const url = pageUrl(link);
            if (!url || isCurrentPage(url) || seen.has(url.href)) {
                return;
            }
            seen.add(url.href);
            appendLink(link.textContent.trim(), url.href);
        });
        const current = document.createElement("li");
        current.textContent = headingLabel(title);
        current.setAttribute("aria-current", "page");
        list.appendChild(current);
        navigation.appendChild(list);
        const header = title.closest(".header");
        header.insertBefore(navigation, header.firstChild);
    }

    function trackNavigation() {
        const tree = document.getElementById("nav-tree-contents");
        if (!tree) {
            return;
        }
        tree.setAttribute("role", "navigation");
        tree.setAttribute("aria-label", "Documentation");
        let queued = false;
        function update() {
            queued = false;
            tree.querySelectorAll(".item > a").forEach(function (toggle) {
                const arrow = toggle.querySelector(".arrow");
                const label = toggle.parentElement.querySelector(".label");
                if (!arrow || !label) {
                    return;
                }
                // Doxygen changes the arrow when its own expansion state changes, including lazy loads.
                const expanded = arrow.textContent.trim() === "\u25bc";
                toggle.setAttribute("role", "button");
                toggle.setAttribute("aria-expanded", String(expanded));
                toggle.setAttribute("aria-label", (expanded ? "Collapse " : "Expand ") + label.textContent.trim());
                arrow.setAttribute("aria-hidden", "true");
                if (!toggle.dataset.readingAccessible) {
                    toggle.dataset.readingAccessible = "true";
                    toggle.addEventListener("keydown", function (event) {
                        // Enter already activates the native anchor.
                        if (event.key === " ") {
                            event.preventDefault();
                            if (!event.repeat) {
                                toggle.click();
                            }
                        }
                    });
                }
            });
            const sync = document.getElementById("nav-sync");
            if (sync) {
                sync.setAttribute("role", "button");
                sync.tabIndex = 0;
                sync.setAttribute("aria-label", "Keep sidebar on the current page");
                sync.setAttribute("aria-pressed", String(sync.classList.contains("sync")));
                if (!sync.dataset.readingAccessible) {
                    sync.dataset.readingAccessible = "true";
                    sync.addEventListener("keydown", function (event) {
                        if (event.key === "Enter" || event.key === " ") {
                            event.preventDefault();
                            if (!event.repeat) {
                                sync.click();
                            }
                        }
                    });
                }
            }
            tree.querySelectorAll(".label > a").forEach(function (link) {
                const url = pageUrl(link);
                const selected = link.closest(".item.selected") && isCurrentPage(url) && !url.hash;
                if (selected && link.getAttribute("aria-current") !== "page") {
                    link.setAttribute("aria-current", "page");
                } else if (!selected && link.hasAttribute("aria-current")) {
                    link.removeAttribute("aria-current");
                }
            });
        }
        function schedule() {
            if (!queued) {
                queued = true;
                queueMicrotask(update);
            }
        }
        new MutationObserver(schedule).observe(document.getElementById("nav-tree") || tree, {
            childList: true, subtree: true, attributes: true, attributeFilter: ["class", "id"]
        });
        window.addEventListener("hashchange", schedule);
        update();
    }

    function enableMobileNavigation() {
        const toggle = document.getElementById("main-menu-state");
        const label = document.querySelector("label.main-menu-btn");
        if (!toggle || !label) {
            return false;
        }
        if (toggle.dataset.readingAccessible) {
            return true;
        }
        toggle.dataset.readingAccessible = "true";
        toggle.setAttribute("aria-label", "Navigation menu");
        toggle.setAttribute("aria-controls", "main-menu");
        label.childNodes.forEach(function (node) {
            if (node.nodeType === Node.TEXT_NODE) {
                node.textContent = " Menu";
            }
        });
        document.addEventListener("keydown", function (event) {
            if (event.key !== "Escape" || event.defaultPrevented || !toggle.checked ||
                !window.matchMedia("(max-width: 767px)").matches) {
                return;
            }
            event.preventDefault();
            toggle.click();
            toggle.focus({ preventScroll: true });
        });
        return true;
    }

    function initialise() {
        const content = document.getElementById("doc-content");
        if (!content || content.dataset.readingReady) {
            return;
        }
        content.dataset.readingReady = "true";
        if (!content.hasAttribute("tabindex")) {
            content.tabIndex = -1;
        }
        if (!document.querySelector("main, [role='main']")) {
            content.setAttribute("role", "main");
        }
        addPageContext(content);
        trackNavigation();
        if (!enableMobileNavigation()) {
            const top = document.getElementById("top");
            if (top) {
                // jQuery's ready callback can create the native menu after DOMContentLoaded and load.
                const menuObserver = new MutationObserver(function () {
                    if (enableMobileNavigation()) {
                        menuObserver.disconnect();
                    }
                });
                menuObserver.observe(top, { childList: true, subtree: true });
            }
        }
        const skip = document.createElement("a");
        skip.className = "reading-skip-link";
        skip.href = "#doc-content";
        skip.textContent = "Skip to content";
        skip.addEventListener("click", function () { content.focus({ preventScroll: true }); });
        document.body.insertBefore(skip, document.body.firstChild);
        document.querySelectorAll(".contents > .textblock").forEach(addContents);
        document.addEventListener("keydown", function (event) {
            if (event.key !== "/" || event.defaultPrevented || event.ctrlKey || event.altKey ||
                event.metaKey || event.isComposing || isEditor(event.target)) {
                return;
            }
            const search = Array.from(document.querySelectorAll("[id='MSearchField']")).find(function (candidate) {
                return !candidate.disabled && !candidate.readOnly && !candidate.closest("[hidden], [inert]") &&
                    candidate.getClientRects().length > 0 && window.getComputedStyle(candidate).visibility === "visible";
            });
            if (!search) {
                return;
            }
            event.preventDefault();
            search.focus();
        });
        enableCopyButtons();
    }

    // The vendor installs its copy controls in an earlier DOMContentLoaded listener.
    if (document.readyState === "loading" || document.readyState === "interactive") {
        document.addEventListener("DOMContentLoaded", initialise, { once: true });
    } else {
        initialise();
    }
    // Also handles scripts inserted after DOMContentLoaded, without polling the document.
    window.addEventListener("load", function () {
        initialise();
        enableCopyButtons();
    }, { once: true });
}());
