export interface GitHubRelease {
    url:              string;
    assets_url:       string;
    upload_url:       string;
    html_url:         string;
    id:               number;
    author:           Author;
    node_id:          string;
    tag_name:         string;
    target_commitish: string;
    name:             string;
    draft:            boolean;
    prerelease:       boolean;
    created_at:       Date;
    published_at:     Date;
    assets:           Asset[];
    tarball_url:      string;
    zipball_url:      string;
    body:             string;
    discussion_url:   string;
    reactions:        Reactions;
}

export interface Asset {
    url:                  string;
    id:                   number;
    node_id:              string;
    name:                 string;
    label:                string;
    uploader:             Author;
    content_type:         ContentType;
    state:                State;
    size:                 number;
    download_count:       number;
    created_at:           Date;
    updated_at:           Date;
    browser_download_url: string;
}

export enum ContentType {
    Raw = "raw",
}

export enum State {
    Uploaded = "uploaded",
}

export interface Author {
    login:               Login;
    id:                  number;
    node_id:             NodeID;
    avatar_url:          string;
    gravatar_id:         string;
    url:                 string;
    html_url:            string;
    followers_url:       string;
    following_url:       string;
    gists_url:           GistsURL;
    starred_url:         string;
    subscriptions_url:   string;
    organizations_url:   string;
    repos_url:           string;
    events_url:          EventsURL;
    received_events_url: string;
    type:                Type;
    site_admin:          boolean;
}

export enum EventsURL {
    HTTPSAPIGithubCOMUsersLizardByteBotEventsPrivacy = "https://api.github.com/users/LizardByte-bot/events{/privacy}",
}

export enum GistsURL {
    HTTPSAPIGithubCOMUsersLizardByteBotGistsGistID = "https://api.github.com/users/LizardByte-bot/gists{/gist_id}",
}

export enum Login {
    LizardByteBot = "LizardByte-bot",
}

export enum NodeID {
    UKgDOBnhkcg = "U_kgDOBnhkcg",
}

export enum Type {
    User = "User",
}

export interface Reactions {
    url:         string;
    total_count: number;
    "+1":        number;
    "-1":        number;
    laugh:       number;
    hooray:      number;
    confused:    number;
    heart:       number;
    rocket:      number;
    eyes:        number;
}
