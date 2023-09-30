#include <string.h>
#include <math.h>
#include <git2.h>

#include <mongoose.h>



#define HTML_SIZE (8*1024*1024)
#define OID_SIZE (8)



static void serve_dir(struct mg_connection *c, struct mg_http_message *hm, const char *dirname) {
    struct mg_http_serve_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.root_dir = dirname;
    mg_http_serve_dir(c, hm, &opts);
}
static void serve_css(struct mg_connection *c, struct mg_http_message *hm, const char *filename) {
  struct mg_http_serve_opts opts = { .mime_types = "css=text/css" };
  mg_http_serve_file(c, hm, filename, &opts);
}
static void serve_html(struct mg_connection *c, struct mg_http_message *hm, const char *filename) {
  struct mg_http_serve_opts opts = { .mime_types = "html=text/html" };
  mg_http_serve_file(c, hm, filename, &opts);
}
static int gitDiffPrintCb(const git_diff_delta *delta, const git_diff_hunk *hunk, const git_diff_line *line, void *payload) {
  struct {
      char *html;
      int *html_len;
    } *userdata = payload;
  //printf("hunk: %s\n", hunk->header);

  if (line->origin == '+' || line->origin == '-' || line->origin == 'H') {

    #define HTML(...) *userdata->html_len += snprintf(userdata->html + *userdata->html_len, HTML_SIZE - *userdata->html_len, __VA_ARGS__)

    /**/ if (line->origin == '+') HTML("<span style=\"color: green;\">%c %*d", line->origin, 6, line->new_lineno);
    else if (line->origin == '-') HTML("<span style=\"color: red;\">%c %*d", line->origin, 6, line->old_lineno);
    else if (line->origin == 'H') HTML("<span>");
    //else                          HTML("<span>%c ", line->origin);

    struct { char c; const char * replacement; }
    replacements[] = {
      { '<', "&lt;" },
      { '>', "&gt;" },
      { '&', "&amp;" },
    };

    for (int i = 0; i < line->content_len; i++) {
      char c = ((char*)line->content)[i];
      bool replaced = false;
      for (int j = 0; j < sizeof(replacements)/sizeof(replacements[0]); j++) {
        if (replacements[j].c == c) {
          size_t len = strlen(replacements[j].replacement);
          memcpy(userdata->html + *userdata->html_len, replacements[j].replacement, len);
          (*userdata->html_len) += len;

          replaced = true;
          break;
        }
      }
      if (! replaced)
        userdata->html[(*userdata->html_len)++] = c;
    }

    HTML("</span>");
  
    #undef HTML

  }

  return 0;
}
static void serve_pdf(struct mg_connection *c, struct mg_http_message *hm, const char *filename) {
  struct mg_http_serve_opts opts =
    { .mime_types = "pdf=application/pdf",
      .extra_headers = "Content-Disposition: inline\r\n"
    };
  mg_http_serve_file(c, hm, filename, &opts);
}
static void serve_git(struct mg_connection *c, struct mg_http_message *hm,
  const char *repo_prefix)
{
  typedef struct mg_str * mg_str_ptr;
  static struct mg_str repo, branch, commit, type, path, file;
  mg_str_ptr caps[] = { &repo, &branch, &commit, &type, &path };

  const int caps_len = sizeof(caps) / sizeof(caps[0]);

  for (int i = 0; i < caps_len; i++)
    caps[i]->len = 0;

  // split repo, branch, commit, type and path
  int state = 0;
  for (int i = 4; i < hm->uri.len; i++) {
    char c = hm->uri.ptr[i];
    if (c == '/' && state <= caps_len-1) {
      caps[state]->ptr = &hm->uri.ptr[i + 1];
      state++;
    }
    else {
      caps[state-1]->len++;
    }
  }

  // if type is file, split path and file
  file.ptr = NULL; file.len = 0;
  if (mg_strcmp(type, mg_str("tree")) != 0) {
    bool foundSlash = false;
    for (int i = path.len - 1; i >= 0; i--) {
      if (path.ptr[i] == '/') {
        file.ptr = path.ptr + i + 1;
        file.len = path.len - i - 1;
        path.len = path.len - file.len - 1;
        foundSlash = true;
        break;
      }
    }
    if (! foundSlash) {
      file = path;
      path.ptr = NULL; path.len = 0;
    }
  }


  // init libgit2
	git_libgit2_init();

  static char html[HTML_SIZE];
  int html_len = 0;
  #define HTML(...) html_len += snprintf(html+html_len,HTML_SIZE-html_len, __VA_ARGS__)

  static char repoStr[128]; snprintf(repoStr, 128, "%s/%.*s", repo_prefix, repo.len, repo.ptr);
  static char branchStr[128]; snprintf(branchStr, 128, "%.*s", branch.len, branch.ptr);
  static char commitStr[128]; snprintf(commitStr, 128, "%.*s", commit.len, commit.ptr);
  static char pathStr[128]; snprintf(pathStr, 128, "%.*s", path.len, path.ptr);

	git_repository * gitrepo = NULL;
	git_repository_open_bare(&gitrepo, repoStr);

	git_revwalk *walk;
	git_revwalk_new(&walk, gitrepo);

  git_annotated_commit *annotated_commit;
  {
  git_reference *ref;
  git_branch_lookup(&ref, gitrepo, branchStr, GIT_BRANCH_LOCAL);
  git_annotated_commit_from_ref(&annotated_commit, gitrepo, ref);
  }

  git_commit *gitcommit;
  git_commit *gitcommitPrev = NULL;
  git_revparse_single((git_object**)&gitcommit, gitrepo, commitStr);
  if (gitcommit == NULL)
    git_commit_lookup(&gitcommit, gitrepo, git_annotated_commit_id(annotated_commit));
  
  git_tree * commitTree;
  git_tree * subtree;
  git_commit_tree(&commitTree, gitcommit);
  if (path.len == 0) {
    subtree = commitTree;
  }
  else {
    const git_tree_entry * pathEntry = git_tree_entry_byname(commitTree, pathStr);
    git_tree_lookup(&subtree, gitrepo, git_tree_entry_id(pathEntry));
  }


  HTML(
    "<html>\n"
    "<head>\n"
    "<style>"
    "body { font-family: monospace; margin: 0; }"
    "div.mainlink { font-family: monospace; font-size: 24pt; }"
    "div.box { border: 1px solid rgb(118,118,118); margin: 10px; overflow: hidden; position: relative; }"
    "div.subbox { margin: 0; width; 100%%; height: 100%%; float: left; }"
    "div.diff { border: 1px solid rgb(118, 118, 118); width: 100%%; height: 100%%; font-family: monospace; overflow-y: scroll; }"
    "</style>\n"
    "</head>\n"
    "<body>\n"
  );

	// Loop over branches

  HTML("<div class=\"box\" style=\"height: 10%%;\">\n");

  HTML("<div class=\"subbox mainlink\" style=\"width: 10%%;\"><a href=\"/git\">git</a></div>\n");

  HTML("<div class=\"subbox\" style=\"width: 20%%; overflow-y: scroll;\">\n");
  
  git_branch_iterator *it;
	if (!git_branch_iterator_new(&it, gitrepo, GIT_BRANCH_LOCAL)) {
		git_reference *ref;
		git_branch_t branchType;
		while (!git_branch_next(&ref, &branchType, it)) {
			// get branch name
			const char *branchName;
			git_branch_name(&branchName, ref);
      
      // get first commit
      git_annotated_commit *annotated_commit = NULL;
      git_annotated_commit_from_ref(&annotated_commit, gitrepo, ref);
      // git_commit *commit;
      // git_commit_lookup(&commit, gitrepo, git_annotated_commit_id(annotated_commit));

      HTML("<a href=\"/git/%.*s/%s/%s/tree\">[branch] %s%s</a><br />\n",
        repo.len, repo.ptr,
        branchName,
        git_oid_tostr_s(git_annotated_commit_id(annotated_commit)),
        (mg_vcmp(&branch, branchName) == 0) ? "> " : "",
        branchName);
    }
  }
  
  HTML("</div>\n");
    
  // Loop over commits

  HTML("<div class=\"subbox\" style=\"width: 70%%; overflow-y: scroll;\">\n");

  git_revwalk_reset(walk);
  git_revwalk_push(walk, git_annotated_commit_id(annotated_commit));
  
  git_oid oid;
  bool currentCommitFound = false;
  while (GIT_ITEROVER != git_revwalk_next(&oid, walk))
  {
    // get commit message
    git_commit *curCommit;
    git_commit_lookup(&curCommit, gitrepo, &oid);

    // check found first, from last loop iteration
    if (currentCommitFound) {
      gitcommitPrev = curCommit;
      currentCommitFound = false;
    }

    // set here for this iteration for HTML, and for next for setting gitcommitPrev :)
    if (git_oid_equal(git_commit_id(gitcommit), git_commit_id(curCommit))) {
      currentCommitFound = true;
    }

    HTML("<a href=\"/git/%.*s/%.*s/%.*s/%.*s%s%.*s%s%.*s\">[%.*s] %s%s</a>",
      repo.len, repo.ptr,
      branch.len, branch.ptr,
      OID_SIZE, git_oid_tostr_s(git_commit_id(curCommit)),
      type.len, type.ptr,
      path.len > 0 ? "/" : "",
      path.len, path.ptr,
      file.len > 0 ? "/" : "",
      file.len, file.ptr,
      OID_SIZE, git_oid_tostr_s(git_commit_id(curCommit)),
      currentCommitFound ? "> " : "",
      git_commit_message(curCommit));
    
    HTML(" (<a href=\"/git/%.*s/%.*s/%.*s/diff\">diff</a>)<br />\n",
      repo.len, repo.ptr,
      branch.len, branch.ptr,
      OID_SIZE, git_oid_tostr_s(git_commit_id(curCommit)));
  }
  
  HTML("</div>\n");

  HTML("</div>\n");
  
  // tree

  HTML("<div class=\"box\" style=\"height: 20%%; overflow-y: scroll;\">\n");
  
  if (path.len > 0)
  {
    int lastSlash = 0;
    for (int i = path.len - 1; i >= 0; i--) {
      if (path.ptr[i] == '/') {
        lastSlash = i;
        break;
      }
    }

    HTML("<a href=\"/git/%.*s/%.*s/%.*s/tree%s%.*s\">[tree] ..</a><br />\n",
      repo.len, repo.ptr,
      branch.len, branch.ptr,
      commit.len, commit.ptr,
      lastSlash > 0 ? "/" : "",
      lastSlash, path.ptr,
      lastSlash);
  }

  size_t count = git_tree_entrycount(subtree);

  for (int i = 0; i < count; i++)
  {
    const git_tree_entry * entry = git_tree_entry_byindex(subtree, i);

    static const char * tree_entry_type_strings[] = {
      "",
      "",
      "tree",
      "blob",
    };

    const char * nameStr = git_tree_entry_name(entry);
    const char * typeStr = tree_entry_type_strings[git_tree_entry_type(entry)];

    HTML("<a href=\"/git/%.*s/%.*s/%.*s/%s%s%.*s%s%s\">%s\t%s%s</a>",
      repo.len, repo.ptr,
      branch.len, branch.ptr,
      commit.len, commit.ptr,
      typeStr,
      path.len > 0 ? "/" : "",
      path.len, path.ptr,
      strlen(nameStr) > 0 ? "/" : "",
      nameStr,
      git_tree_entry_type(entry) == GIT_OBJECT_TREE ? "&#x1F4C1" : "&#x1F4C4",
      (mg_vcmp(&file, nameStr) == 0) ? "> " : "",
      nameStr);
    
    
    HTML("<br />\n");
  }
  
  HTML("</div>\n");
  
  // blob

  if (mg_strcmp(type, mg_str("blob")) == 0)
  {
    static char fileStr[128]; snprintf(fileStr, 128, "%.*s", file.len, file.ptr);
    const git_tree_entry * entry = git_tree_entry_byname(subtree, fileStr);

    if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
      git_blob *blob;
      int res =
        git_tree_entry_to_object((git_object**)&blob, gitrepo, entry);

      if (res == 0) {
        git_object_size_t rawsize = (int)git_blob_rawsize(blob);
        const void * rawcontent = git_blob_rawcontent(blob);
        bool isBinary = git_blob_is_binary(blob);

        if (! isBinary) {
          HTML("<pre style=\"height: calc(70%% - 46px); margin: 10px;\">"
               "<div readonly class=\"diff\">");

          struct { char c; const char *replacement; }
          replacements[] = {
              {'<', "&lt;"},
              {'>', "&gt;"},
              {'&', "&amp;"},
          };

          for (int i = 0; i < rawsize; i++)
          {
            char c = ((char *)rawcontent)[i];
            bool replaced = false;
            for (int j = 0; j < sizeof(replacements) / sizeof(replacements[0]); j++)
            {
              if (replacements[j].c == c)
              {
                size_t len = strlen(replacements[j].replacement);
                memcpy(html + html_len, replacements[j].replacement, len);
                html_len += len;

                replaced = true;
                break;
              }
            }
            if (!replaced)
              html[html_len++] = c;
          }
          HTML("</div></pre>\n");
        }
        else {
          HTML("<pre>Binary file :[</pre>\n");
        }
      }
      else {
        HTML("<pre>File not found :{</pre>\n");
      }
    }
    else {
      switch (git_tree_entry_type(entry)) {
        	case GIT_OBJECT_ANY:       HTML("<pre> Error loading GIT_OBJECT_ANY! >:( </pre>\n"); break;
          case GIT_OBJECT_INVALID:   HTML("<pre> Error loading GIT_OBJECT_INVALID! >:( </pre>\n"); break;
          case GIT_OBJECT_COMMIT:    HTML("<pre> Error loading GIT_OBJECT_COMMIT! >:( </pre>\n"); break;
          case GIT_OBJECT_TREE:      HTML("<pre> Error loading GIT_OBJECT_TREE! >:( </pre>\n"); break;
          case GIT_OBJECT_BLOB:      HTML("<pre> Error loading GIT_OBJECT_BLOB! >:( </pre>\n"); break;
          case GIT_OBJECT_TAG:       HTML("<pre> Error loading GIT_OBJECT_TAG! >:( </pre>\n"); break;
          case GIT_OBJECT_OFS_DELTA: HTML("<pre> Error loading GIT_OBJECT_OFS_DELTA! >:( </pre>\n"); break;
          case GIT_OBJECT_REF_DELTA: HTML("<pre> Error loading GIT_OBJECT_REF_DELTA! >:( </pre>\n"); break;
      }
    }
  }
  else if (mg_strcmp(type, mg_str("diff")) == 0) {
    if (gitcommitPrev != NULL) {
      git_diff * diff;
      git_tree * commitTreePrev;
      git_tree * subtreePrev;
      git_commit_tree(&commitTreePrev, gitcommitPrev);
      if (path.len == 0) {
        subtreePrev = commitTreePrev;
      }
      else {
        const git_tree_entry * pathEntry = git_tree_entry_byname(commitTreePrev, pathStr);
        git_tree_lookup(&subtreePrev, gitrepo, git_tree_entry_id(pathEntry));
      }

      git_diff_tree_to_tree(&diff, gitrepo, subtreePrev, subtree, NULL);
      struct {
        char *html;
        int *html_len;
      } userdata;
      userdata.html = html;
      userdata.html_len = &html_len;

      HTML("<pre style=\"height: calc(70%% - 46px); margin: 10px;\">"
           "<div readonly class=\"diff\">");
      
      git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, gitDiffPrintCb, &userdata);
      
      HTML("</div></pre>\n");
    }
    else {
      git_diff * diff;
      git_diff_tree_to_tree(&diff, gitrepo, NULL, subtree, NULL);
      struct {
        char *html;
        int *html_len;
      } userdata;
      userdata.html = html;
      userdata.html_len = &html_len;

      HTML("<pre style=\"height: calc(70%% - 46px); margin: 10px;\">"
           "<div readonly class=\"diff\">");
      
      git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, gitDiffPrintCb, &userdata);
      
      HTML("</div></pre>\n");
    }
  }
  
  HTML("</body>\n</html>");

  #undef HTML

  mg_http_reply(c, 200, NULL, "%s", html);
}
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_ACCEPT) {
    struct mg_tls_opts opts = {
      .cert = "./certs/cert1.pem",
      .certkey = "./certs/privkey1.pem",
    };
    mg_tls_init(c, &opts);
  }
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    //printf("serving %.*s\n", hm->message.len, hm->message.ptr);

    /**/ if (mg_http_match_uri(hm, "/")) {
      serve_html(c, hm, "html/index.html");
    }
    else if (mg_http_match_uri(hm, "/html/style.css")) {
      serve_css(c, hm, "html/style.css");
    }
    else if (mg_http_match_uri(hm, "/portfolio")) {
      serve_html(c, hm, "html/portfolio.html");
    }

    else if (mg_http_match_uri(hm, "/projects/sensor/slides")) {
      serve_html(c, hm, "projects/sensor/Vortrag.pdf");
    }
    else if (strncmp(hm->uri.ptr, "/lol", 4) == 0) {
      static struct mg_http_serve_opts opts;
      opts.root_dir = "/lol=/mnt/hdd/obs";
      mg_http_serve_dir(c, hm, &opts);
    }
    else if (strncmp(hm->uri.ptr, "/clips", 4) == 0) {
      static struct mg_http_serve_opts opts;
      opts.root_dir = "/clips=/mnt/hdd/clips";
      mg_http_serve_dir(c, hm, &opts);
    }
    else if (strncmp(hm->uri.ptr, "/share", 6) == 0) {
      static struct mg_http_serve_opts opts;
      opts.root_dir = "/share=/mnt/hdd/";
      mg_http_serve_dir(c, hm, &opts);
    }
    // else if (mg_http_match_uri(hm, "/git")) {
    //   mg_http_reply(c, 307, "Location: https://patdog.de/git/HEAD\r\n", "");
    // }
    else if (mg_http_match_uri(hm, "/git/*/*/*/*#")) {
      serve_git(c, hm, "/home/pi/git");
    }
    else if (mg_http_match_uri(hm, "/git#")) {
      static char html[1024*10];
      int htmlSize = 0;
      htmlSize += snprintf(html + htmlSize, 1024*10 - htmlSize, "<html>\n");
      FILE * f = popen("ls -t /home/pi/git | grep \\\\.git", "r");
      char name[128];
      while (fscanf(f, "%127[^\n]\n", name) == 1)
        htmlSize += snprintf(html + htmlSize, 1024*10 - htmlSize, "<a href=\"/git/%s/main/HEAD/tree\">%s</a><br />\n", name, name);
      htmlSize += snprintf(html + htmlSize, 1024*10 - htmlSize, "</html>\n");
      pclose(f);

      mg_http_reply(c, 200, NULL, "%s", html);
    }
    //else if (strncmp(hm->uri.ptr, "/hdd", 4) == 0) {
    //  static struct mg_http_serve_opts opts;
    //  opts.root_dir = "/hdd=/mnt/hdd";
    //  mg_http_serve_dir(c, hm, &opts);
    //}
    else if (mg_http_match_uri(hm, "/projects/sensor/pdf")) {
      serve_html(c, hm, "projects/sensor/AusarbeitungSensornetze.pdf");
    }

    else if (mg_http_match_uri(hm, "/projects/ba")) {
      serve_html(c, hm, "projects/ba/Thesis.pdf");
    }

    else if (mg_http_match_uri(hm, "/projects/bezier")) {
      serve_html(c, hm, "projects/bezier/index.html");
    }

    else if (mg_http_match_uri(hm, "/projects/ik")) {
      serve_html(c, hm, "projects/ik/index.html");
    }
    
    else if (mg_http_match_uri(hm, "/projects/sss/exposee")) {
      serve_html(c, hm, "projects/sss/Exposee.pdf");
    }
    else if (mg_http_match_uri(hm, "/projects/sss/pdf")) {
      serve_html(c, hm, "projects/sss/RealtimeSubsurfaceScattering.pdf");
    }
    
    else if (mg_http_match_uri(hm, "/projects/se/slides")) {
      serve_html(c, hm, "projects/se/Presentation_Symbolic_Execution.pdf");
    }
    else if (mg_http_match_uri(hm, "/projects/se/pdf")) {
      serve_html(c, hm, "projects/se/Symbolic_Execution_Paper_Preview.pdf");
    }

    else if (mg_http_match_uri(hm, "/projects/cloth")) {
      serve_html(c, hm, "projects/cloth/index.html");
    }
    else if (mg_http_match_uri(hm, "/projects/cloth/pdf")) {
      serve_pdf(c, hm, "projects/cloth/pdf/Cloth_simulation_Paper.pdf");
    }
    else if (mg_http_match_uri(hm, "/projects/cloth/*/*")) {
      serve_dir(c, hm, ".");
    }

    else if (mg_http_match_uri(hm, "/wol")) {
      system("/home/pi/wol.sh");
      puts("Sending WoL");
      mg_http_reply(c, 200, NULL, "WoL sent");
    }
  }
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);

  mg_http_listen(&mgr, "[::]:443", fn, &mgr);

  while (true)
    mg_mgr_poll(&mgr, 1000);

  mg_mgr_free(&mgr);
  return 0;
}

