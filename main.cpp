/*
Single-file Qt 5.12 demo GitHub client (WITHOUT GitHub CLI).
This version replaces all `gh` CLI usage with direct HTTPS REST calls to the GitHub API.
Features:
 - Search GitHub repositories for a username via GitHub REST API v3
 - Clone repositories (git clone)
 - Check for remote updates (git fetch + git status parsing)
 - Pull latest (git pull)
 - Detect changed files (git status --porcelain)
 - Show file diffs (git diff)
 - Commit & push local changes (git add -A, git commit -m, git push)

NOTE:
 - To avoid rate-limits, you should provide a GitHub personal access token.
 - The simplest path: set an environment variable `GITHUB_TOKEN` before launching.
 - For Qt 5.12, we use QtNetwork/QNetworkAccessManager.

Build (example using qmake):
  QT += widgets network
  CONFIG += c++11
  SOURCES += main.cpp
*/

#include <QApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QLabel>
#include <QFileDialog>
#include <QProcess>
#include <QMessageBox>
#include <QSplitter>
#include <QInputDialog>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

static bool runCommand(const QString &program, const QStringList &args, QString &stdoutOut, QString &stderrOut, int timeoutMs = 120000)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) { stderrOut = "Failed to start"; return false; }
    if (!proc.waitForFinished(timeoutMs)) { proc.kill(); stderrOut = "Timeout"; return false; }
    stdoutOut = proc.readAllStandardOutput();
    stderrOut = proc.readAllStandardError();
    return proc.exitCode() == 0;
}

class GitHubClient : public QWidget {
    Q_OBJECT
public:
    GitHubClient(QWidget *parent=nullptr) : QWidget(parent), net(new QNetworkAccessManager(this))
    {
        setupUi();
        connectSignals();
        appendLog("Qt GitHub Client demo (REST API version) started.");
        token = qgetenv("GITHUB_TOKEN");
        if(token.isEmpty()) appendLog("WARNING: No GITHUB_TOKEN set — GitHub API rate limit will be LOW.");
    }

private:
    QLineEdit *usernameEdit;
    QPushButton *searchBtn;
    QPushButton *cloneBtn;
    QPushButton *chooseDirBtn;
    QPushButton *refreshLocalBtn;
    QPushButton *checkUpdatesBtn;
    QPushButton *pullBtn;
    QPushButton *diffBtn;
    QPushButton *pushBtn;

    QListWidget *repoList;
    QListWidget *fileList;
    QPlainTextEdit *diffView;
    QPlainTextEdit *logView;

    QString localBaseDir;
    QString token;
    QNetworkAccessManager *net;

    void setupUi()
    {
        auto *main = new QVBoxLayout(this);
        auto *top = new QHBoxLayout();
        usernameEdit = new QLineEdit();
        usernameEdit->setPlaceholderText("GitHub username");
        searchBtn = new QPushButton("Search Repos");
        chooseDirBtn = new QPushButton("Set Clone Dir");
        cloneBtn = new QPushButton("Clone Selected");
        top->addWidget(new QLabel("User:"));
        top->addWidget(usernameEdit);
        top->addWidget(searchBtn);
        top->addWidget(chooseDirBtn);
        top->addWidget(cloneBtn);
        main->addLayout(top);

        auto *split = new QSplitter(Qt::Horizontal);

        auto *left = new QWidget();
        auto *ll = new QVBoxLayout(left);
        repoList = new QListWidget();
        ll->addWidget(new QLabel("Repositories"));
        ll->addWidget(repoList);
        refreshLocalBtn = new QPushButton("Refresh Local");
        checkUpdatesBtn = new QPushButton("Check Updates");
        pullBtn = new QPushButton("Pull");
        ll->addWidget(refreshLocalBtn);
        ll->addWidget(checkUpdatesBtn);
        ll->addWidget(pullBtn);
        split->addWidget(left);

        auto *mid = new QWidget();
        auto *ml = new QVBoxLayout(mid);
        fileList = new QListWidget();
        ml->addWidget(new QLabel("Files"));
        ml->addWidget(fileList);
        diffBtn = new QPushButton("Show Diff");
        ml->addWidget(diffBtn);
        split->addWidget(mid);

        auto *right = new QWidget();
        auto *rl = new QVBoxLayout(right);
        diffView = new QPlainTextEdit(); diffView->setReadOnly(true);
        rl->addWidget(new QLabel("Diff"));
        rl->addWidget(diffView);
        pushBtn = new QPushButton("Commit & Push");
        rl->addWidget(pushBtn);
        split->addWidget(right);

        main->addWidget(split);

        logView = new QPlainTextEdit(); logView->setReadOnly(true);
        main->addWidget(new QLabel("Log"));
        main->addWidget(logView);

        localBaseDir = QDir::homePath() + "/qt-gh-clones";
        appendLog("Default clone directory: " + localBaseDir);
    }

    void connectSignals()
    {
        connect(searchBtn, &QPushButton::clicked, this, &GitHubClient::onSearchRepos);
        connect(cloneBtn, &QPushButton::clicked, this, &GitHubClient::onCloneSelected);
        connect(chooseDirBtn, &QPushButton::clicked, this, &GitHubClient::onChooseDir);
        connect(repoList, &QListWidget::currentTextChanged, this, &GitHubClient::onRepoSelected);
        connect(refreshLocalBtn, &QPushButton::clicked, this, &GitHubClient::onRefreshLocal);
        connect(checkUpdatesBtn, &QPushButton::clicked, this, &GitHubClient::onCheckUpdates);
        connect(pullBtn, &QPushButton::clicked, this, &GitHubClient::onPullSelected);
        connect(diffBtn, &QPushButton::clicked, this, &GitHubClient::onShowDiff);
        connect(pushBtn, &QPushButton::clicked, this, &GitHubClient::onPushIfChanged);
    }

    void appendLog(const QString &t)
    {
        logView->appendPlainText("["+QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")+"] " + t);
    }

private slots:

    //=========================== API REQUEST ================================
    void onSearchRepos()
    {
        QString user = usernameEdit->text().trimmed();
        if(user.isEmpty()){ QMessageBox::warning(this,"Input","Username required"); return; }
        appendLog("Searching repos via GitHub REST API...");

        QUrl url(QString("https://api.github.com/users/%1/repos?per_page=100").arg(user));
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, "QtGitHubClient");
        if(!token.isEmpty()) req.setRawHeader("Authorization", "token " + token.toUtf8());

        QNetworkReply *r = net->get(req);
        connect(r, &QNetworkReply::finished, this, [this, r](){ handleRepoListReply(r); });
    }

    void handleRepoListReply(QNetworkReply *r)
    {
        QByteArray data = r->readAll();
        if(r->error()!=QNetworkReply::NoError){
            appendLog("API error: " + r->errorString());
            QMessageBox::warning(this,"API error",r->errorString());
            r->deleteLater();
            return;
        }
        r->deleteLater();

        QJsonDocument doc = QJsonDocument::fromJson(data);
        if(!doc.isArray()){
            appendLog("Unexpected API JSON");
            return;
        }
        repoList->clear();
        for(const QJsonValue &v : doc.array()){
            QJsonObject o = v.toObject();
            QString name = o.value("name").toString();
            QString ssh  = o.value("ssh_url").toString();
            QListWidgetItem *it = new QListWidgetItem(name);
            it->setData(Qt::UserRole, ssh);
            repoList->addItem(it);
        }
        appendLog(QString("Loaded %1 repos.").arg(repoList->count()));
    }

    //=========================== LOCAL OPERATIONS ===========================
    void onChooseDir()
    {
        QString d = QFileDialog::getExistingDirectory(this,"Choose Clone Directory",localBaseDir);
        if(!d.isEmpty()){ localBaseDir = d; appendLog("Clone dir set: "+d); }
    }

    void onCloneSelected()
    {
        auto sel = repoList->selectedItems();
        if(sel.isEmpty()){ QMessageBox::information(this,"Select","Select a repo"); return; }
        for(QListWidgetItem *it : sel){
            QString name = it->text();
            QString ssh  = it->data(Qt::UserRole).toString();
            if(ssh.isEmpty()){ appendLog("No SSH URL for "+name); continue; }
            QString target = QDir(localBaseDir).filePath(name);
            if(QDir(target).exists()){ appendLog("Already exists: "+target); continue; }
            appendLog("Cloning "+ssh);
            QString out, err;
            bool ok = runCommand("git", {"clone", ssh, target}, out, err, 0);
            appendLog(out + "y" + err);
            if(!ok) QMessageBox::warning(this,"Clone failed",err);
        }
        onRefreshLocal();
    }

    void onRepoSelected(){ onRefreshLocal(); }

    void onRefreshLocal()
    {
        fileList->clear();
        QListWidgetItem *it = repoList->currentItem(); if(!it) return;
        QString name = it->text(); QString path = QDir(localBaseDir).filePath(name);
        if(!QDir(path).exists()){ appendLog("Local missing: "+path); return; }

        QString out, err;
        runCommand("git", {"-C", path, "status", "--porcelain"}, out, err, 20000);
        QStringList lines = out.split("",QString::SkipEmptyParts);
        if(lines.isEmpty()) fileList->addItem("Working tree clean");
        else for(auto &l: lines) fileList->addItem(l);

        appendLog("Refreshed local state.");
    }

    void onCheckUpdates()
    {
        QListWidgetItem *it = repoList->currentItem(); if(!it){ QMessageBox::information(this,"Select","Select repo"); return; }
        QString name = it->text(); QString p = QDir(localBaseDir).filePath(name);
        appendLog("Fetching remote...");

        QString out, err;
        runCommand("git", {"-C", p, "fetch"}, out, err, 60000);
        appendLog(out+err);

        QString brOut, brErr;
        runCommand("git", {"-C", p, "rev-parse", "--abbrev-ref", "HEAD"}, brOut, brErr);
        QString branch = brOut.trimmed();

        QString cntOut, cntErr;
        bool ok = runCommand("git", {"-C", p, "rev-list", "--left-right", "--count", QString("origin/%1...HEAD").arg(branch)}, cntOut, cntErr);
        if(ok){
            QStringList parts = cntOut.split(QRegExp("[	 ]"),QString::SkipEmptyParts);
            if(parts.size()>=2)
                QMessageBox::information(this,"Remote",QString("Behind: %1 Ahead: %2").arg(parts[0],parts[1]));
        } else QMessageBox::information(this,"Remote",out+err);
    }

    void onPullSelected()
    {
        QListWidgetItem *it = repoList->currentItem(); if(!it) return;
        QString name = it->text(); QString p = QDir(localBaseDir).filePath(name);
        appendLog("Pulling...");
        QString out, err;
        runCommand("git", {"-C", p, "pull"}, out, err, 120000);
        appendLog(out+err);
        QMessageBox::information(this,"Pull",out+err);
        onRefreshLocal();
    }

    void onShowDiff()
    {
        QListWidgetItem *repo = repoList->currentItem();
        QListWidgetItem *file = fileList->currentItem();
        if(!repo || !file){ QMessageBox::information(this,"Select","Select file"); return; }
        QString name = repo->text(); QString p = QDir(localBaseDir).filePath(name);
        QString entry = file->text();

        QString path;
        if(entry.contains("	")) path = entry.section("	",1);
        else if(entry.size()>3) path = entry.mid(3).trimmed();
        if(path.isEmpty()) path=entry;

        QString out, err;
        runCommand("git", {"-C", p, "diff", "--", path}, out, err, 20000);
        diffView->setPlainText(out+err);
    }

    void onPushIfChanged()
    {
        QListWidgetItem *it = repoList->currentItem(); if(!it) return;
        QString name = it->text(); QString p = QDir(localBaseDir).filePath(name);
        QString out, err;
        runCommand("git", {"-C", p, "status", "--porcelain"}, out, err);
        if(out.trimmed().isEmpty()){ QMessageBox::information(this,"Clean","Nothing to push"); return; }

        bool ok;
        QString msg = QInputDialog::getText(this,"Commit message","Message:",QLineEdit::Normal,"Update",&ok);
        if(!ok) return;

        runCommand("git", {"-C", p, "add", "-A"}, out, err);
        runCommand("git", {"-C", p, "commit", "-m", msg}, out, err);
        runCommand("git", {"-C", p, "push"}, out, err);
        QMessageBox::information(this,"Pushed",out+err);
        onRefreshLocal();
    }
};



int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    GitHubClient w;
    w.resize(1100,700);
    w.setWindowTitle("Qt GitHub Client REST API");
    w.show();
    return app.exec();
}
#include "main.moc"
