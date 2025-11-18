/*
Single-file Qt 5.12 demo GitHub client.
Features:
 - Search GitHub repositories for a username (uses `gh repo list <user> --json name,sshUrl`)
 - Clone repositories (git clone)
 - Check for remote updates (git fetch + git status parsing)
 - Pull latest (git pull)
 - Detect changed files (git status --porcelain)
 - Show file diffs (git diff)
 - Commit & push local changes (git add -A, git commit -m, git push)
 - Simple UI with repo list, file list and diff/log viewer

Requirements (for demo):
 - Qt 5.12 (Widgets module)
 - `git` available on PATH
 - `gh` (GitHub CLI) available on PATH and authenticated (optional, required for repo search)

Build (example using qmake):
 1) Create simple .pro file:
    QT += widgets
    CONFIG += c++11
    SOURCES += main.cpp

 2) qmake && make

Run and interact with the UI.

Note: This is a demo. It purposefully keeps error handling simple so you can read the flow. Use at your own risk; the push path will commit and push to the repo you choose.
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
#include <QFileInfo>
#include <QTimer>

// Utility helper: run a command and capture output (async-friendly sync call with timeout)
static bool runCommand(const QString &program, const QStringList &args, QString &stdoutOut, QString &stderrOut, int timeoutMs = 120000)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) {
        stderrOut = "Failed to start process";
        return false;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(3000);
        stderrOut = "Process timed out";
        return false;
    }
    stdoutOut = QString::fromLocal8Bit(proc.readAllStandardOutput());
    stderrOut = QString::fromLocal8Bit(proc.readAllStandardError());
    return proc.exitCode() == 0;
}

class GitHubClient : public QWidget {
    Q_OBJECT
public:
    GitHubClient(QWidget *parent = nullptr) : QWidget(parent)
    {
        setupUi();
        connectSignals();
        appendLog("Qt GitHub Client demo started.");
        appendLog("Requirements: `git` and `gh` (GitHub CLI) on PATH. Authenticate gh using `gh auth login` if you want to search your repos.");
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

    QString localBaseDir; // where clones will go

    void setupUi()
    {
        auto *mainLayout = new QVBoxLayout(this);

        auto *topRow = new QHBoxLayout();
        usernameEdit = new QLineEdit();
        usernameEdit->setPlaceholderText("GitHub username or org (e.g. octocat)");
        searchBtn = new QPushButton("Search Repos");
        chooseDirBtn = new QPushButton("Set Clone Dir");
        cloneBtn = new QPushButton("Clone Selected");
        topRow->addWidget(new QLabel("User:"));
        topRow->addWidget(usernameEdit);
        topRow->addWidget(searchBtn);
        topRow->addWidget(chooseDirBtn);
        topRow->addWidget(cloneBtn);
        mainLayout->addLayout(topRow);

        auto *split = new QSplitter(Qt::Horizontal);

        auto *leftPanel = new QWidget();
        auto *leftLayout = new QVBoxLayout(leftPanel);
        repoList = new QListWidget();
        leftLayout->addWidget(new QLabel("Repositories"));
        leftLayout->addWidget(repoList);

        refreshLocalBtn = new QPushButton("Refresh Local State");
        checkUpdatesBtn = new QPushButton("Check Remote Updates");
        pullBtn = new QPushButton("Pull Selected");
        leftLayout->addWidget(refreshLocalBtn);
        leftLayout->addWidget(checkUpdatesBtn);
        leftLayout->addWidget(pullBtn);

        split->addWidget(leftPanel);

        auto *middlePanel = new QWidget();
        auto *middleLayout = new QVBoxLayout(middlePanel);
        fileList = new QListWidget();
        middleLayout->addWidget(new QLabel("Files / Working Tree"));
        middleLayout->addWidget(fileList);
        diffBtn = new QPushButton("Show Diff for Selected File");
        middleLayout->addWidget(diffBtn);
        split->addWidget(middlePanel);

        auto *rightPanel = new QWidget();
        auto *rightLayout = new QVBoxLayout(rightPanel);
        diffView = new QPlainTextEdit();
        diffView->setReadOnly(true);
        rightLayout->addWidget(new QLabel("Diff / Output"));
        rightLayout->addWidget(diffView);
        pushBtn = new QPushButton("Commit & Push If Changed");
        rightLayout->addWidget(pushBtn);
        split->addWidget(rightPanel);

        split->setSizes({200, 200, 400});

        mainLayout->addWidget(split, 1);

        logView = new QPlainTextEdit();
        logView->setReadOnly(true);
        logView->setMaximumHeight(180);
        mainLayout->addWidget(new QLabel("Log"));
        mainLayout->addWidget(logView);

        // sensible defaults
        localBaseDir = QDir::homePath() + "/qt-gh-clones";
        appendLog(QString("Default clone dir: %1").arg(localBaseDir));
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

    void appendLog(const QString &text)
    {
        QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        logView->appendPlainText(QString("[%1] %2").arg(stamp, text));
    }

private slots:
    void onSearchRepos()
    {
        QString user = usernameEdit->text().trimmed();
        if (user.isEmpty()) {
            QMessageBox::warning(this, "Input required", "Please enter a GitHub username or organization.");
            return;
        }
        appendLog(QString("Searching repos for %1 using gh CLI...").arg(user));

        // Use gh repo list <user> --limit 200 --json name,sshUrl
        QString out, err;
        bool ok = runCommand("gh", {"repo", "list", user, "--limit", "200", "--json", "name,sshUrl"}, out, err, 120000);
        if (!ok) {
            appendLog(QString("gh failed: %1").arg(err));
            QMessageBox::warning(this, "gh failed", "Failed to list repos via gh. Ensure gh is installed and authenticated. Error: " + err);
            return;
        }
        // Parse JSON
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            appendLog(QString("Failed to parse gh JSON: %1 -- falling back to plain output").arg(parseErr.errorString()));
            // fallback: gh repo list <user> gives text lines like 'name\tSSH\n'
            repoList->clear();
            for (const QString &line : out.split('\n', QString::SkipEmptyParts)) {
                repoList->addItem(line.trimmed());
            }
            return;
        }
        repoList->clear();
        if (!doc.isArray()) {
            appendLog("Unexpected gh json (not array)");
            return;
        }
        QJsonArray arr = doc.array();
        for (const QJsonValue &val : arr) {
            if (!val.isObject()) continue;
            QJsonObject o = val.toObject();
            QString name = o["name"].toString();
            QString ssh = o["sshUrl"].toString();
            QListWidgetItem *it = new QListWidgetItem(name);
            it->setData(Qt::UserRole, ssh);
            repoList->addItem(it);
        }
        appendLog(QString("Found %1 repositories.").arg(arr.size()));
    }

    void onChooseDir()
    {
        QString dir = QFileDialog::getExistingDirectory(this, "Select base directory for clones", localBaseDir);
        if (!dir.isEmpty()) {
            localBaseDir = dir;
            appendLog(QString("Set clone base dir: %1").arg(localBaseDir));
        }
    }

    void onCloneSelected()
    {
        QList<QListWidgetItem*> items = repoList->selectedItems();
        if (items.isEmpty()) {
            QMessageBox::information(this, "Select a repo", "Please select a repository to clone.");
            return;
        }
        for (QListWidgetItem *it : items) {
            QString name = it->text();
            QString ssh = it->data(Qt::UserRole).toString();
            if (ssh.isEmpty()) {
                appendLog(QString("No ssh url for %1, skipping").arg(name));
                continue;
            }
            QString target = QDir(localBaseDir).filePath(name);
            if (QDir(target).exists()) {
                appendLog(QString("Target %1 already exists, skipping clone").arg(target));
                continue;
            }
            appendLog(QString("Cloning %1 -> %2").arg(ssh, target));
            QString out, err;
            bool ok = runCommand("git", {"clone", ssh, target}, out, err, 0); // no timeout
            appendLog(out);
            if (!ok) {
                appendLog(QString("Clone failed: %1").arg(err));
                QMessageBox::warning(this, "Clone failed", QString("Clone failed: %1").arg(err));
            } else {
                appendLog("Clone succeeded.");
            }
        }
        // refresh file list for currently selected repo
        onRefreshLocal();
    }

    void onRepoSelected(const QString &text)
    {
        Q_UNUSED(text);
        onRefreshLocal();
    }

    void onRefreshLocal()
    {
        fileList->clear();
        QListWidgetItem *it = repoList->currentItem();
        if (!it) return;
        QString name = it->text();
        QString localPath = QDir(localBaseDir).filePath(name);
        if (!QDir(localPath).exists()) {
            appendLog(QString("Local repo not found: %1").arg(localPath));
            return;
        }
        // list changed files: git status --porcelain
        QString out, err;
        bool ok = runCommand("git", {"-C", localPath, "status", "--porcelain"}, out, err, 20000);
        if (!ok) appendLog(QString("git status failed: %1").arg(err));
        QStringList lines = out.split('\n', QString::SkipEmptyParts);
        if (lines.isEmpty()) {
            fileList->addItem("Working tree clean");
        } else {
            for (const QString &ln : lines) fileList->addItem(ln);
        }
        // show tracked files (quick): git ls-files (first 200)
        QString lsOut, lsErr;
        bool ok2 = runCommand("git", {"-C", localPath, "ls-files", "--modified", "--others", "--exclude-standard"}, lsOut, lsErr, 20000);
        if (!ok2) {
            appendLog(QString("git ls-files failed: %1").arg(lsErr));
        } else {
            // add a separator and show a few files
            fileList->addItem("--- Working tree files (modified/others) ---");
            QStringList files = lsOut.split('\n', QString::SkipEmptyParts);
            int show = qMin(200, files.size());
            for (int i=0;i<show;i++) fileList->addItem(files.at(i));
            if (files.size() > show) fileList->addItem(QString("... (%1 more) ...").arg(files.size()-show));
        }
        appendLog("Refreshed local working tree view.");
    }

    void onCheckUpdates()
    {
        QListWidgetItem *it = repoList->currentItem();
        if (!it) {
            QMessageBox::information(this, "Select repo", "Select a repository first.");
            return;
        }
        QString name = it->text();
        QString localPath = QDir(localBaseDir).filePath(name);
        if (!QDir(localPath).exists()) {
            QMessageBox::warning(this, "Local repo missing", "Repository not cloned locally: " + localPath);
            return;
        }
        appendLog(QString("Checking for remote updates for %1").arg(name));
        QString out, err;
        // fetch remote
        bool ok = runCommand("git", {"-C", localPath, "fetch"}, out, err, 60000);
        appendLog(out);
        if (!ok) appendLog(QString("git fetch failed: %1").arg(err));

        // get current branch
        QString branchOut, branchErr;
        bool okb = runCommand("git", {"-C", localPath, "rev-parse", "--abbrev-ref", "HEAD"}, branchOut, branchErr, 10000);
        QString branch = okb ? branchOut.trimmed() : "master";

        // check behind/ahead count
        QString cntOut, cntErr;
        bool okc = runCommand("git", {"-C", localPath, "rev-list", "--left-right", "--count", QString::fromUtf8("origin/%1...HEAD").arg(branch)}, cntOut, cntErr, 10000);
        if (okc) {
            // output like "<behind> <ahead>"
            QStringList parts = cntOut.split('\t', QString::SkipEmptyParts);
            if (parts.isEmpty()) parts = cntOut.split(' ', QString::SkipEmptyParts);
            if (parts.size() >= 2) {
                QString behind = parts.at(0).trimmed();
                QString ahead = parts.at(1).trimmed();
                appendLog(QString("Branch %1 is behind origin/%1 by %2 commits, ahead by %3 commits.").arg(branch, behind, ahead));
                QMessageBox::information(this, "Remote check", QString("Branch %1: behind %2, ahead %3").arg(branch, behind, ahead));
            } else {
                appendLog(QString("rev-list output unexpected: %1").arg(cntOut));
            }
        } else {
            appendLog(QString("rev-list failed, falling back to status parsing: %1").arg(cntErr));
            QString statusOut, statusErr;
            runCommand("git", {"-C", localPath, "status", "-uno"}, statusOut, statusErr, 10000);
            appendLog(statusOut);
            QMessageBox::information(this, "Remote check", statusOut);
        }
    }

    void onPullSelected()
    {
        QListWidgetItem *it = repoList->currentItem();
        if (!it) {
            QMessageBox::information(this, "Select repo", "Select a repository first.");
            return;
        }
        QString name = it->text();
        QString localPath = QDir(localBaseDir).filePath(name);
        if (!QDir(localPath).exists()) {
            QMessageBox::warning(this, "Local repo missing", "Repository not cloned locally: " + localPath);
            return;
        }
        appendLog(QString("Pulling %1").arg(name));
        QString out, err;
        bool ok = runCommand("git", {"-C", localPath, "pull"}, out, err, 120000);
        appendLog(out);
        if (!ok) appendLog(QString("git pull failed: %1").arg(err));
        QMessageBox::information(this, "Pull finished", out + "\n" + err);
        onRefreshLocal();
    }

    void onShowDiff()
    {
        QListWidgetItem *repoIt = repoList->currentItem();
        QListWidgetItem *fileIt = fileList->currentItem();
        if (!repoIt || !fileIt) {
            QMessageBox::information(this, "Select items", "Select a repository and a file (or changed entry) to show diff.");
            return;
        }
        QString repoName = repoIt->text();
        QString localPath = QDir(localBaseDir).filePath(repoName);
        QString fileEntry = fileIt->text();
        // If fileEntry is status line like ' M path/to/file' or '?? path', extract path
        QString path;
        // try to detect 'name\t' or 'XY path'
        if (fileEntry.contains('\t')) {
            path = fileEntry.section('\t', 1);
        } else if (fileEntry.startsWith("---")) {
            appendLog("Cannot show diff for separator entry.");
            return;
        } else {
            // if starts with status code two chars then space
            if (fileEntry.size() > 3 && (fileEntry[0].isSpace() || fileEntry[1].isSpace())) {
                path = fileEntry.mid(3).trimmed();
            } else {
                path = fileEntry.trimmed();
            }
        }
        if (path.isEmpty()) path = fileEntry.trimmed();
        appendLog(QString("Showing diff for %1:%2").arg(repoName, path));
        QString out, err;
        bool ok = runCommand("git", {"-C", localPath, "diff", "--", path}, out, err, 20000);
        if (!ok) appendLog(QString("git diff failed: %1").arg(err));
        diffView->setPlainText(out + "\n" + err);
    }

    void onPushIfChanged()
    {
        QListWidgetItem *it = repoList->currentItem();
        if (!it) {
            QMessageBox::information(this, "Select repo", "Select a repository first.");
            return;
        }
        QString name = it->text();
        QString localPath = QDir(localBaseDir).filePath(name);
        if (!QDir(localPath).exists()) {
            QMessageBox::warning(this, "Local repo missing", "Repository not cloned locally: " + localPath);
            return;
        }
        appendLog(QString("Checking for local changes in %1").arg(name));
        QString statusOut, statusErr;
        runCommand("git", {"-C", localPath, "status", "--porcelain"}, statusOut, statusErr, 10000);
        if (statusOut.trimmed().isEmpty()) {
            appendLog("No changes to commit.");
            QMessageBox::information(this, "No changes", "Working tree clean — nothing to commit.");
            return;
        }
        // Offer commit message
        bool ok;
        QString msg = QInputDialog::getText(this, "Commit message", "Commit message:", QLineEdit::Normal, "Auto update from Qt GitHub client", &ok);
        if (!ok) return;

        appendLog("Adding all changes...");
        QString outAdd, errAdd;
        bool okAdd = runCommand("git", {"-C", localPath, "add", "-A"}, outAdd, errAdd, 20000);
        appendLog(outAdd + "\n" + errAdd);
        if (!okAdd) {
            appendLog("git add failed.");
            QMessageBox::warning(this, "Add failed", errAdd);
            return;
        }
        appendLog("Committing...");
        QString outCi, errCi;
        bool okCi = runCommand("git", {"-C", localPath, "commit", "-m", msg}, outCi, errCi, 20000);
        appendLog(outCi + "\n" + errCi);
        if (!okCi) {
            // if there's nothing to commit maybe commit failed
            if (errCi.contains("nothing to commit")) {
                appendLog("Nothing to commit after add — aborting.");
                QMessageBox::information(this, "Nothing to commit", "No changes to commit.");
                return;
            }
            QMessageBox::warning(this, "Commit failed", errCi);
            return;
        }
        appendLog("Pushing to remote...");
        QString outPush, errPush;
        bool okPush = runCommand("git", {"-C", localPath, "push"}, outPush, errPush, 60000);
        appendLog(outPush + "\n" + errPush);
        if (!okPush) {
            appendLog("git push failed: " + errPush);
            QMessageBox::warning(this, "Push failed", errPush);
            return;
        }
        QMessageBox::information(this, "Success", "Committed and pushed changes.");
        onRefreshLocal();
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    GitHubClient w;
    w.setWindowTitle("Qt GitHub Client (demo)");
    w.resize(1100, 700);
    w.show();
    return app.exec();
}

#include "main.moc"
