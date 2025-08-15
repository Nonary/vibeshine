const getStoredTheme = () => localStorage.getItem('theme')
const setStoredTheme = theme => localStorage.setItem('theme', theme)

export const getPreferredTheme = () => {
    const storedTheme = getStoredTheme()
    if (storedTheme) {
        return storedTheme
    }

    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

const setTheme = theme => {
    if (theme === 'auto') {
        document.documentElement.setAttribute(
            'data-bs-theme',
            (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')
        )
    } else {
        document.documentElement.setAttribute('data-bs-theme', theme)
    }
}

export const showActiveTheme = (theme, focus = false) => {
    const themeSwitcher = document.querySelector('#bd-theme')
    if (!themeSwitcher) return
    document.querySelectorAll('[data-bs-theme-value]').forEach(element => {
        element.classList.remove('active')
        element.setAttribute('aria-pressed', 'false')
    })
    const btnToActive = document.querySelector(`[data-bs-theme-value="${theme}"]`)
    if (btnToActive) {
        btnToActive.classList.add('active')
        btnToActive.setAttribute('aria-pressed', 'true')
    }
    // Dispatch event so Vue components can reactively update icon
    window.dispatchEvent(new CustomEvent('sunshine-theme-change', { detail: { theme } }))
    if (focus) themeSwitcher.focus()
}

export function setupThemeToggleListener() {
    document.querySelectorAll('[data-bs-theme-value]')
        .forEach(toggle => {
            toggle.addEventListener('click', () => {
                const theme = toggle.getAttribute('data-bs-theme-value')
                setStoredTheme(theme)
                setTheme(theme)
                showActiveTheme(theme, true)
            })
        })

    showActiveTheme(getPreferredTheme(), false)
}

export function loadAutoTheme() {
    (() => {
        'use strict'

        setTheme(getPreferredTheme())

        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
            const storedTheme = getStoredTheme()
            if (storedTheme !== 'light' && storedTheme !== 'dark') {
                setTheme(getPreferredTheme())
            }
        })

        window.addEventListener('DOMContentLoaded', () => {
            showActiveTheme(getPreferredTheme())
        })
    })()
}
