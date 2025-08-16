type Theme = 'light' | 'dark' | 'auto';

const getStoredTheme = (): string | null => localStorage.getItem('theme');
const setStoredTheme = (theme: Theme): void => localStorage.setItem('theme', theme);

export const getPreferredTheme = (): Theme => {
    const storedTheme = getStoredTheme();
    if (storedTheme && (storedTheme === 'light' || storedTheme === 'dark' || storedTheme === 'auto')) {
        return storedTheme as Theme;
    }

    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
};

const setTheme = (theme: Theme): void => {
    if (theme === 'auto') {
        document.documentElement.setAttribute(
            'data-bs-theme',
            (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')
        );
    } else {
        document.documentElement.setAttribute('data-bs-theme', theme);
    }
};

export const showActiveTheme = (theme: Theme, focus = false): void => {
    const themeSwitcher = document.querySelector('#bd-theme');
    if (!themeSwitcher) return;
    
    document.querySelectorAll('[data-bs-theme-value]').forEach((element: Element) => {
        element.classList.remove('active');
        element.setAttribute('aria-pressed', 'false');
    });
    
    const btnToActive = document.querySelector(`[data-bs-theme-value="${theme}"]`);
    if (btnToActive) {
        btnToActive.classList.add('active');
        btnToActive.setAttribute('aria-pressed', 'true');
    }
    
    // Dispatch event so Vue components can reactively update icon
    window.dispatchEvent(new CustomEvent('sunshine-theme-change', { detail: { theme } }));
    if (focus) (themeSwitcher as HTMLElement).focus();
};

export function setupThemeToggleListener(): void {
    document.querySelectorAll('[data-bs-theme-value]')
        .forEach((toggle: Element) => {
            toggle.addEventListener('click', () => {
                const theme = toggle.getAttribute('data-bs-theme-value') as Theme;
                setStoredTheme(theme);
                setTheme(theme);
                showActiveTheme(theme, true);
            });
        });

    showActiveTheme(getPreferredTheme(), false);
}

export function loadAutoTheme(): void {
    (() => {
        'use strict';

        setTheme(getPreferredTheme());

        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
            const storedTheme = getStoredTheme();
            if (storedTheme !== 'light' && storedTheme !== 'dark') {
                setTheme(getPreferredTheme());
            }
        });

        window.addEventListener('DOMContentLoaded', () => {
            showActiveTheme(getPreferredTheme());
        });
    })();
}